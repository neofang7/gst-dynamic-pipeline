#include <gst/gst.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>

/*
 * gst-launch-1.0 icamerasrc device-name=ov13858-uf af-mode=2 printfps=true !
 * video/x-raw,format=NV12,width=1920,height=1080 !
 * queue !
 * videocrop top=35 left=1080 right=4 bottom=20 !
 * videoscale ! video/x-raw,wdith=1920,height=1080 !
 * videoconvert ! ximagesink
 */
typedef struct
{
    GstPad *blockpad;

    GstElement *src;
    GstElement *src_filter;
    GstElement *q1;
    GstElement *crop;
    GstElement *crop_filter;
    GstElement *scale;
    GstElement *scale_filter;
    GstElement *convert;
    GstElement *sink;
}Element;

Element *el = NULL;
GstElement *pipeline = NULL;

typedef struct {
    int top;
    int bottom;
    int left;
    int right;
} CropSize;

typedef struct {
    int w;
    int h;
} ScaleSize;

static CropSize crop_size = {0, 0, 0, 0};
static CropSize min_crop = {270, 270, 480, 480};

GstPadProbeReturn
downstream_event_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    unsigned int method = 0;

    if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_DATA(info)) != GST_EVENT_EOS)
        return GST_PAD_PROBE_PASS;
    gst_pad_remove_probe(pad, GST_PAD_PROBE_INFO_ID(info));

    gst_element_set_state(el->crop, GST_STATE_NULL);
    g_object_set(el->crop, "top", crop_size.top, NULL);
    g_object_set(el->crop, "bottom", crop_size.bottom, NULL);
    g_object_set(el->crop, "left", crop_size.left, NULL);
    g_object_set(el->crop, "right", crop_size.right, NULL);

    gst_element_set_state(el->crop, GST_STATE_PLAYING);

    return GST_PAD_PROBE_DROP;
}

GstPadProbeReturn
blockpad_probe_cb(GstPad *blockpad, GstPadProbeInfo *info, gpointer user_data)
{
    GstPad *srcpad, *sinkpad;

    gst_pad_remove_probe(blockpad, GST_PAD_PROBE_INFO_ID(info));

    srcpad = gst_element_get_static_pad(el->crop, "src");
    gst_pad_add_probe(srcpad, (GstPadProbeType)(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
                      downstream_event_probe_cb, user_data, NULL);
    gst_object_unref(srcpad);

    sinkpad = gst_element_get_static_pad(el->crop, "sink");
    gst_pad_send_event(sinkpad, gst_event_new_eos());
    gst_object_unref(sinkpad);

    return GST_PAD_PROBE_OK;
}

int delete_pipeline()
{
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    pipeline = NULL;
    return 0;
}

int create_pipeline(int w, int h)
{
    GstCaps *caps = NULL;
    el = (Element*)malloc(sizeof(Element));
    if (el == NULL) {
        printf("Error: Failed to create elements.\n");
        return -1;
    }
    memset(el, 0, sizeof(Element));

    pipeline = gst_pipeline_new("zoom_pipeline");
    assert (pipeline != NULL);

    el->src = gst_element_factory_make("icamerasrc", "mipi_src");
    el->src_filter = gst_element_factory_make("capsfilter", "src_filter");
    el->q1 = gst_element_factory_make("queue", "queue");
    el->crop = gst_element_factory_make("videocrop", "crop");
    el->crop_filter = gst_element_factory_make("capsfilter", "crop_filter");
    el->scale = gst_element_factory_make("videoscale", "scale");
    el->scale_filter = gst_element_factory_make("capsfilter", "scale_fitler");
    el->convert = gst_element_factory_make("videoconvert", "convert");
    el->sink = gst_element_factory_make("ximagesink", "imagesink");

    assert(el->src != NULL);
    assert(el->src_filter != NULL);
    assert(el->q1 != NULL);
    assert(el->crop != NULL);
    assert(el->crop_filter != NULL);
    assert(el->scale != NULL && el->scale_filter != NULL);
    assert(el->convert != NULL && el->sink != NULL);

    //Set icamerasrc prop.
    g_object_set(el->src, "device-name", 0, NULL);
    g_object_set(el->src, "af-mode", 2, NULL);
    //Set icamerasrc caps.
    caps = gst_caps_new_simple("video/x-raw",
                "format", G_TYPE_STRING, "NV12",
                "width", G_TYPE_INT, 1280,
                "height", G_TYPE_INT, 720,
                NULL);
    g_object_set(el->src_filter, "caps", caps, NULL);
    gst_caps_unref(caps);

    //set default crop: 0 0 0 0
    caps = gst_caps_new_simple("video/x-raw",
                "top", G_TYPE_INT, crop_size.top,
                "bottom", G_TYPE_INT, crop_size.bottom,
                "left", G_TYPE_INT, crop_size.left,
                "right", G_TYPE_INT, crop_size.right,
                NULL);
    g_object_set(el->crop_filter, "caps", caps, NULL);
    gst_caps_unref(caps);

    caps = gst_caps_new_simple("video/x-raw",
                "width", G_TYPE_INT, 1280,
                "height", G_TYPE_INT, 720,
                NULL);
    g_object_set(el->scale_filter, "caps", caps, NULL);
    gst_caps_unref(caps);

    el->blockpad = gst_element_get_static_pad(el->q1, "src");

    //add and link
    gst_bin_add_many(GST_BIN(pipeline), el->src, el->src_filter, el->q1, el->crop, el->crop_filter,
        el->scale, el->scale_filter, el->convert, el->sink, NULL);
    gst_element_link_many(el->src, el->src_filter, el->q1, el->crop, el->crop_filter,
        el->scale, el->scale_filter, el->convert, el->sink, NULL);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    printf("thread playing...\n");
    return 0;
}

void set_crop(void *args)
{
    gst_pad_add_probe(el->blockpad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                    blockpad_probe_cb, (gpointer)args, NULL);

    return;
}

void* pipeline_thread_cb(void *args)
{
    create_pipeline(1280, 720);
    return NULL;
}

int main(int argc, char *argv[])
{
    char c;
    gst_init(NULL, NULL);

    pthread_t pipeline_thread;
    pthread_create(&pipeline_thread, NULL, pipeline_thread_cb, NULL);
    printf("pause...\n");

    while (true) {
        c = getchar();
        if (c == 'q') {
            printf("Quit...\n");
            break;
        }

        if (c == 'a') {
            if (crop_size.top >= min_crop.top  && crop_size.bottom >= min_crop.bottom
                && crop_size.left >= min_crop.left && crop_size.right >= min_crop.right)
                printf("Max size, cannot zoom out\n");
            else {
                crop_size.top += 27;
                crop_size.bottom += 27;
                crop_size.left += 48;
                crop_size.right += 48;
                set_crop(NULL);
            }
        } else if (c == 'd') {
            if (crop_size.top == 0) {
                printf("Min size, cannot zoom in\n");
            } else {
                crop_size.top -= 27;
                crop_size.bottom -= 27;
                crop_size.left -= 48;
                crop_size.right -= 48;
                set_crop(NULL);
            }
        } else {
            //Nothing to do.
            printf("Invalid char. %c\n", c);
        }
    }

    void *tret = NULL;
    delete_pipeline();
    pthread_join(pipeline_thread, &tret);

    return 0;
}