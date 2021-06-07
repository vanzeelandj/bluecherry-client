/*
 * Copyright 2010-2019 Bluecherry, LLC
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "RtspStreamWorker.h"
#include "RtspStreamFrame.h"
#include "RtspStreamFrameFormatter.h"
#include "RtspStreamFrameQueue.h"
#include "core/BluecherryApp.h"
#include <QDebug>
#include <QCoreApplication>
#include <QThread>
#include "core/VaapiHWAccel.h"
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/mathematics.h>
}

#define ASSERT_WORKER_THREAD() Q_ASSERT(QThread::currentThread() == thread())

static const int maxDecodeErrors = 3;

int rtspStreamInterruptCallback(void *opaque)
{
    RtspStreamWorker *worker = (RtspStreamWorker *)opaque;
    return worker->shouldInterrupt();
}

RtspStreamWorker::RtspStreamWorker(QSharedPointer<RtspStreamFrameQueue> &shared_queue, bool hwaccelerated, QObject *parent)
    : QObject(parent), m_ctx(0),
      m_videoCodecCtx(0), m_audioCodecCtx(0),
      m_frame(0), m_decodeErrorsCnt(0),
      m_videoStreamIndex(-1), m_audioStreamIndex(-1),
      m_audioEnabled(false),
      m_hwaccelEnabled(hwaccelerated),
      m_frameWidthHint(-1), m_frameHeightHint(-1),
      m_cancelFlag(false), m_autoDeinterlacing(true),
      m_frameQueue(new RtspStreamFrameQueue(6))
{
    shared_queue = m_frameQueue;
}

RtspStreamWorker::~RtspStreamWorker()
{
    if (!m_ctx)
        return;

    av_frame_free(&m_frame);

    avcodec_close(m_videoCodecCtx);
    avcodec_close(m_audioCodecCtx);

    avcodec_free_context(&m_videoCodecCtx);
    avcodec_free_context(&m_audioCodecCtx);

    startInterruptableOperation(5);
    avformat_close_input(&m_ctx);
}

void RtspStreamWorker::setUrl(const QUrl &url)
{
    m_url = url;
}

void RtspStreamWorker::setAutoDeinterlacing(bool autoDeinterlacing)
{
    m_autoDeinterlacing = autoDeinterlacing;
    if (m_frameFormatter)
        m_frameFormatter->setAutoDeinterlacing(autoDeinterlacing);
}

bool RtspStreamWorker::shouldInterrupt() const
{
    if (m_cancelFlag)
        return true;

    if (m_timeout < QDateTime::currentDateTime())
        return true;

    return false;
}

void RtspStreamWorker::run()
{
    ASSERT_WORKER_THREAD();

    // Prevent concurrent invocations
    if (m_ctx)
        return;

    if (setup())
        processStreamLoop();

    deleteLater();
}

void RtspStreamWorker::processStreamLoop()
{
    bool abortFlag = false;
    while (!m_cancelFlag && !abortFlag)
    {
        if (m_threadPause.shouldPause())
            pause();
        abortFlag = !processStream();
    }
}

bool RtspStreamWorker::processStream()
{
    bool ok = false;
    AVPacket packet = readPacket(&ok);
    if (!ok)
        return false;

    bool result = processPacket(packet);
    av_packet_unref(&packet);
    return result;
}

AVPacket RtspStreamWorker::readPacket(bool *ok)
{
    if (ok)
        *ok = true;

    AVPacket packet;
    startInterruptableOperation(30);
    int re = av_read_frame(m_ctx, &packet);
    if (0 == re)
        return packet;

    emit fatalError(QString::fromLatin1("Reading error: %1").arg(errorMessageFromCode(re)));
    av_packet_unref(&packet);

    if (ok)
        *ok = false;
    return packet;
}

bool RtspStreamWorker::processPacket(struct AVPacket packet)
{
    emit bytesDownloaded(packet.size);

    while (packet.size > 0)
    {
        if (packet.stream_index == m_audioStreamIndex)
        {
            if (!m_audioEnabled)
                return true;

            AVFrame *frame = extractAudioFrame(packet);

            if (frame)
            {
                //feed samples to audio player
                int bytesNum = 0;

                //bytesNum is set to linesize because only first plane is played in case of planar sample format
                av_samples_get_buffer_size(&bytesNum, frame->channels, frame->nb_samples, (enum AVSampleFormat)frame->format, 0);

                emit audioSamplesAvailable(frame->data[0], frame->nb_samples, bytesNum);
            }
        }

        if (packet.stream_index == m_videoStreamIndex)
        {
            AVFrame *frame = extractVideoFrame(packet);

            if (frame)
                processVideoFrame(frame);

            if (m_decodeErrorsCnt >= maxDecodeErrors)
                return false;

            break; //always expect single frame in video packets
        }

        if (packet.stream_index != m_audioStreamIndex && packet.stream_index != m_videoStreamIndex)
            break;//drop packet from unknown stream
    }

    return true;
}

AVFrame * RtspStreamWorker::extractAudioFrame(AVPacket &packet)
{
    startInterruptableOperation(5);

    int ret = avcodec_send_packet(m_audioCodecCtx, &packet);

    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        return 0;

    if (ret == 0)
        packet.size = 0;

    ret = avcodec_receive_frame(m_audioCodecCtx, m_frame);

    if (ret < 0)
        return 0;

    return m_frame;
}

AVFrame * RtspStreamWorker::extractVideoFrame(AVPacket &packet)
{
    startInterruptableOperation(5);

    int ret = avcodec_send_packet(m_videoCodecCtx, &packet);

    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        goto fail;

    if (ret == 0)
        packet.size = 0;

    ret = avcodec_receive_frame(m_videoCodecCtx, m_frame);

    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        goto fail;

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return 0;

    if (m_hwaccelEnabled)
    {
#if defined(Q_OS_LINUX)
        if (m_videoCodecCtx->opaque)
        {
            ret = VaapiHWAccel::retrieveData(m_videoCodecCtx, m_frame);

            if (ret < 0)
            {
                goto fail;
            }
        }
        else
        {
            m_hwaccelEnabled = false;
            emit hwAccelDisabled();
        }
#endif
    }
    m_decodeErrorsCnt = 0; //reset error counter if extracting frame was successful

    return m_frame;

fail:

    m_decodeErrorsCnt++;

    if (m_decodeErrorsCnt >= maxDecodeErrors)
        emit fatalError(QString::fromLatin1("Decoding error: %1").arg(errorMessageFromCode(ret)));

    return 0;
}

void RtspStreamWorker::processVideoFrame(struct AVFrame *rawFrame)
{
    Q_ASSERT(m_frameFormatter);
    startInterruptableOperation(5);
    m_frameQueue->enqueue(m_frameFormatter->formatFrame(rawFrame, m_frameWidthHint, m_frameHeightHint));
}

QString RtspStreamWorker::errorMessageFromCode(int errorCode)
{
    char error[512];
    av_strerror(errorCode, error, sizeof(error));
    return QString::fromLatin1(error);
}

bool RtspStreamWorker::setup()
{
    ASSERT_WORKER_THREAD();

    m_ctx = avformat_alloc_context();
    m_ctx->interrupt_callback.callback = rtspStreamInterruptCallback;
    m_ctx->interrupt_callback.opaque = this;

    AVDictionary *options = createOptions();
    bool prepared = prepareStream(&m_ctx, options);
    av_dict_free(&options);

    if (prepared)
    {
        m_frameFormatter.reset(new RtspStreamFrameFormatter(m_ctx->streams[m_videoStreamIndex]));
        m_frameFormatter->setAutoDeinterlacing(m_autoDeinterlacing);
        m_frame = av_frame_alloc();
    }
    else if (m_ctx)
    {
        avformat_close_input(&m_ctx);
        m_ctx = 0;
    }

    return prepared;
}

bool RtspStreamWorker::prepareStream(AVFormatContext **context, AVDictionary *options)
{
    if (!openInput(context, options))
        return false;

    if (!findStreamInfo(*context, options))
        return false;

    if (!openCodecs(*context, options))
        return false;

    return true;
}

AVDictionary * RtspStreamWorker::createOptions() const
{
    AVDictionary *options = 0;

    av_dict_set(&options, "threads", "1", 0);
    //av_dict_set(&options, "allowed_media_types", "-audio-data", 0);
    av_dict_set(&options, "max_delay", QByteArray::number(qint64(0.3*AV_TIME_BASE)).constData(), 0);
    /* Because the server always starts streams on a keyframe, we don't need any time here.
     * If the first frame is not a keyframe, this could result in failures or corruption. */
    av_dict_set(&options, "analyzeduration", "0", 0);
    /* Only TCP is supported currently; speed up connection by trying that first */
    av_dict_set(&options, "rtsp_transport", "tcp", 0);

    return options;
}

bool RtspStreamWorker::openInput(AVFormatContext **context, AVDictionary *options)
{
    AVDictionary *optionsCopy = 0;
    av_dict_copy(&optionsCopy, options, 0);
    startInterruptableOperation(20);
    int errorCode = avformat_open_input(context, qPrintable(m_url.toString()), NULL, &optionsCopy);
    av_dict_free(&optionsCopy);

    if (errorCode < 0)
    {
        emit fatalError(QString::fromLatin1("Open error: %1").arg(errorMessageFromCode(errorCode)));
        return false;
    }

    return true;
}

bool RtspStreamWorker::findStreamInfo(AVFormatContext* context, AVDictionary* options)
{
    AVDictionary **streamOptions = createStreamsOptions(context, options);
    startInterruptableOperation(20);
    int errorCode = avformat_find_stream_info(context, streamOptions);
    destroyStreamOptions(context, streamOptions);

    if (errorCode < 0)
    {
        emit fatalError(QString::fromLatin1("Find stream error: %1").arg(errorMessageFromCode(errorCode)));
        return false;
    }

    return true;
}

AVDictionary ** RtspStreamWorker::createStreamsOptions(AVFormatContext *context, AVDictionary *options) const
{
    AVDictionary **streamOptions = 0;
    streamOptions = new AVDictionary*[context->nb_streams];
    for (unsigned int i = 0; i < context->nb_streams; ++i)
    {
        streamOptions[i] = 0;
        av_dict_copy(&streamOptions[i], options, 0);
    }

    return streamOptions;
}

void RtspStreamWorker::destroyStreamOptions(AVFormatContext *context, AVDictionary** streamOptions)
{
    if (!streamOptions)
        return;

    for (unsigned int i = 0; i < context->nb_streams; ++i)
        av_dict_free(&streamOptions[i]);
    delete[] streamOptions;
}

bool RtspStreamWorker::openCodecs(AVFormatContext *context, AVDictionary *options)
{
    for (unsigned int i = 0; i < context->nb_streams; i++)
    {
        qDebug() << "processing stream id " << i;

        AVCodecContext *avctx = avcodec_alloc_context3(NULL);

        if (!avctx)
        {
            qDebug() << "RtspStreamWorker: failed to allocate memory for AVCodecContext";
            continue;
        }

        AVStream *stream = context->streams[i];

        if (m_hwaccelEnabled)
        {
#if defined(Q_OS_LINUX)
            if (stream->codecpar->codec_type==AVMEDIA_TYPE_VIDEO && bcApp->vaapi->isAvailable())
            {
                avctx->get_format = VaapiHWAccel::get_format;

                //TODO: filter out low-res video streams?

                qDebug() << "trying to use VAAPI acceleration for video stream decoding";
                av_log_set_level(AV_LOG_VERBOSE);
            }
#endif
        }

        bool codecOpened = openCodec(stream, avctx, options);
        if (!codecOpened)
        {
            qDebug() << "RtspStream: cannot find decoder for stream" << i << "codec" <<
                        stream->codecpar->codec_id;
            avcodec_free_context(&avctx);
            continue;
        }

        if (stream->codecpar->codec_type==AVMEDIA_TYPE_VIDEO)
        {
            m_videoStreamIndex = i;
            m_videoCodecCtx = avctx;
        }

        if (stream->codecpar->codec_type==AVMEDIA_TYPE_AUDIO)
        {
            m_audioStreamIndex = i;
            m_audioCodecCtx = avctx;
        }

        char info[512];
        avcodec_string(info, sizeof(info), avctx, 0);
        qDebug() << "RtspStream: stream #" << i << ":" << info;
    }

    if (m_audioStreamIndex > -1)
    {
        qDebug() << "audio stream time base " << m_audioCodecCtx->time_base.num
                 << "/"
                 << m_audioCodecCtx->time_base.den;

        emit audioFormat(m_audioCodecCtx->sample_fmt,
                         m_audioCodecCtx->channels,
                         m_audioCodecCtx->sample_rate);
    }

    qDebug() << "video stream index: " << m_videoStreamIndex;
    qDebug() << "audio steam index: " << m_audioStreamIndex;

    if (m_videoStreamIndex == -1)
    {
        emit fatalError(QString::fromLatin1("Failed to open video stream"));
        return false;
    }
    else if (context->streams[m_videoStreamIndex]->codecpar->format == AV_PIX_FMT_NONE)
    {
        emit fatalError(QString::fromLatin1("Unknown pixel format in video stream"));
        return false;
    }

    return true;
}

bool RtspStreamWorker::openCodec(AVStream *stream, AVCodecContext *avctx, AVDictionary *options)
{
    if (avcodec_parameters_to_context(avctx, stream->codecpar) < 0)
        return false;

    startInterruptableOperation(5);
    AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);

    if (codec == NULL)
        return false;

    AVDictionary *optionsCopy = 0;
    av_dict_copy(&optionsCopy, options, 0);
    startInterruptableOperation(5);
    int errorCode = avcodec_open2(avctx, codec, &optionsCopy);
    av_dict_free(&optionsCopy);

    return 0 == errorCode;
}

void RtspStreamWorker::startInterruptableOperation(int timeoutInSeconds)
{
    m_timeout = QDateTime::currentDateTime().addSecs(timeoutInSeconds);
}

RtspStreamFrame * RtspStreamWorker::frameToDisplay()
{
    if (m_cancelFlag || !m_frameQueue)
        return 0;

    return m_frameQueue.data()->dequeue();
}

void RtspStreamWorker::setFrameSizeHint(int width, int height)
{
    m_frameWidthHint = width;
    m_frameHeightHint = height;
}

void RtspStreamWorker::stop()
{
    m_cancelFlag = true;
    m_threadPause.setPaused(false);
}

void RtspStreamWorker::setPaused(bool paused)
{
    if (!m_ctx)
        return;

    m_threadPause.setPaused(paused);
}

void RtspStreamWorker::pause()
{
    av_read_pause(m_ctx);
    m_threadPause.pause();
    av_read_play(m_ctx);
}
