#include "VaapiHWAccel.h"

#if defined(Q_OS_LINUX)

#include <QDebug>
#include <QProcessEnvironment>
#include <QByteArray>

extern "C"
{
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixdesc.h>
#include <libavutil/frame.h>
#include <libavcodec/vaapi.h>
}

#define VAAPIHWACCEL_SURFACES_NUM 0

VaapiHWAccel *VaapiHWAccel::m_instance = 0;

int VaapiHWAccel::retrieveData(AVCodecContext *s, AVFrame *input)
{
    AVFrame *output = 0;
    int ret;

    if (s->opaque == NULL)
        return 0;

    Q_ASSERT(input->format == AV_PIX_FMT_VAAPI);

    output = av_frame_alloc();

    if (!output)
        return AVERROR(ENOMEM);

    output->format = s->sw_pix_fmt;

    if (output->format == AV_PIX_FMT_YUVJ420P)
        output->format = AV_PIX_FMT_YUV420P;

    ret = av_hwframe_transfer_data(output, input, 0);

    if (ret < 0)
    {
        qDebug() << "Failed to transfer data to output frame: " << ret;
        goto fail;
    }

    ret = av_frame_copy_props(output, input);

    if (ret < 0) {
        av_frame_unref(output);
        goto fail;
    }

    av_frame_unref(input);
    av_frame_move_ref(input, output);
    av_frame_free(&output);

    return 0;

fail:
    if (output)
        av_frame_free(&output);
    return ret;
}

int VaapiHWAccel::decoderInit(AVCodecContext *s)
{
    qDebug() << "initializing VAAPI decoder...";

    s->pix_fmt = s->sw_pix_fmt;
    s->hw_device_ctx = av_buffer_ref(m_instance->m_hw_device_ctx);

    s->opaque = (void*)1; //hardware acceleration is enabled for this AVCodecContext instance

    return 1;
}

enum AVPixelFormat VaapiHWAccel::get_format(AVCodecContext *s, const enum AVPixelFormat *pix_fmts)
{
    Q_ASSERT(m_instance != 0);

    const enum AVPixelFormat *p;

    char pixdescbuf[64];

    s->opaque = 0;

    qDebug() << "get_format() callback for AVCodecContext " << s;

    for (p = pix_fmts; *p != -1; p++)
    {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);

        qDebug() << "checking pix fmt " << av_get_pix_fmt_string(pixdescbuf, 63, *p);

        if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
                    break;

        if (*p == AV_PIX_FMT_VAAPI)
        {
            qDebug() << "Trying to initialize VAAPI decoder";
            if (!decoderInit(s))
            {
                qDebug() << "Failed to initialize VAAPI decoder for stream";
                return s->sw_pix_fmt;
            }

            break;
        }

    }

    qDebug() << "returning pix_fmt " << av_get_pix_fmt_string(pixdescbuf, 63, *p);
    return *p;
}

VaapiHWAccel::VaapiHWAccel()
    : m_available(false), m_hw_device_ctx(0)
{
    int err;
    const char *device;

    Q_ASSERT(m_instance == 0);

#if defined(Q_WS_X11)
    QProcessEnvironment env;
    QByteArray x11display;

    env = QProcessEnvironment::systemEnvironment();

    if (env.contains(QLatin1String("DISPLAY")))
    {
        x11display = env.value(QLatin1String("DISPLAY")).toAscii();
        qDebug() << "VAAPI - using X11 display " << x11display;
        device = x11display.constData();
    }
    else
#endif
        device = "/dev/dri/renderD128";

    qDebug() << "trying to create VAAPI device context \"" << device << "\"";

    err = av_hwdevice_ctx_create(&m_hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                 device, NULL, 0);
    if (err < 0)
    {
        qDebug() << "Failed to create VAAPI device context";
        m_available = false;
        return;
    }

    m_available = true;
    m_instance = this;
}

VaapiHWAccel::~VaapiHWAccel()
{
    Q_ASSERT(m_instance != 0);
}

#endif
