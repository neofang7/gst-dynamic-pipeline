#include <gst/gst.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <string.h>
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
    GstPad *queue_blockpad;

    GstElement *src;
    GstElement *src_filter;
    GstElement *q1;
    GstElement *tee;
    GstElement *crop;
    GstElement *crop_filter;
    GstElement *scale;
    GstElement *scale_filter;
    GstElement *convert;
    GstElement *sink;
}Element_preview;

typedef struct
{
    GstPad *tee_imagepad;
    GstPad *queue_imagepad;
    //GstCaps *appsink_caps;

    GstElement *src_filter;
    GstElement *queue_image;
   // GstElement *tee;
    GstElement *crop;
    GstElement *crop_filter;
    GstElement *scale;
    GstElement *scale_filter;
    GstElement *convert;
    GstElement *sink;
    //GstElement *appsink_convert;
    //GstElement *app_sink;
}Element_image;

typedef struct
{
    GstPad *tee_videopad;
    GstPad *queue_videopad;

    GstElement *src_filter_v;
    GstElement *queue_video;
    GstElement *crop_v;
    GstElement *crop_filter_v;
    GstElement *scale_v;
    GstElement *scale_filter_v;
    GstElement *encoder;
    GstElement *muxer;
    GstElement *filesink;
    GstElement *convert;

}Element_video;

Element_preview *el = NULL;
Element_image *el_image = NULL;
Element_video *el_video = NULL;

GstElement *pipeline = NULL;

#define CAPS "video/x-raw,format=RGB,pixel-aspect-ratio=1/1"

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

GstElement *appsink_queue, *appsink_video_scale, *appsink_video_capsfilter, *appsink_convert, *app_sink;
GstCaps *appsink_video_capsfilter_caps,*appsink_caps;

static gint counter = 0;
static gboolean recording = FALSE;
static char *file_path;


static CropSize crop_size = {0, 0, 0, 0};
static CropSize min_crop = {270, 270, 480, 480};

static CropSize crop_image_size = {0, 0, 0, 0};

static GstPadProbeReturn unlink_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
	g_print("Unlinking...");
	GstPad *sinkpad;
	sinkpad = gst_element_get_static_pad (el_video->queue_video, "sink");
	gst_pad_unlink (el_video->tee_videopad, sinkpad);
	gst_object_unref (sinkpad);

	gst_element_send_event(el_video->encoder, gst_event_new_eos());

	sleep(1);
	gst_bin_remove(GST_BIN (pipeline), el_video->queue_video);
	gst_bin_remove(GST_BIN (pipeline), el_video->encoder);
	gst_bin_remove(GST_BIN (pipeline), el_video->muxer);
	gst_bin_remove(GST_BIN (pipeline), el_video->filesink);

	gst_element_set_state(el_video->queue_video, GST_STATE_NULL);
	gst_element_set_state(el_video->encoder, GST_STATE_NULL);
	gst_element_set_state(el_video->muxer, GST_STATE_NULL);
	gst_element_set_state(el_video->filesink, GST_STATE_NULL);

	gst_object_unref(el_video->queue_video);
	gst_object_unref(el_video->encoder);
	gst_object_unref(el_video->muxer);
	gst_object_unref(el_video->filesink);

	gst_element_release_request_pad (el->tee, el_video->tee_videopad);
	gst_object_unref (el_video->tee_videopad);

	g_print("Unlinked\n");

	return GST_PAD_PROBE_REMOVE;
}

void stopRecording() {
	g_print("stopRecording\n");
	gst_pad_add_probe(el_video->tee_videopad, GST_PAD_PROBE_TYPE_IDLE, unlink_cb, NULL, (GDestroyNotify) g_free);
	recording = FALSE;
}

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

GstPadProbeReturn
downstream__video_event_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    unsigned int method = 0;

    if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_DATA(info)) != GST_EVENT_EOS)
        return GST_PAD_PROBE_PASS;
    gst_pad_remove_probe(pad, GST_PAD_PROBE_INFO_ID(info));

    gst_element_set_state(el_video->crop_v, GST_STATE_NULL);
    g_object_set(el_video->crop_v, "top", crop_size.top, NULL);
    g_object_set(el_video->crop_v, "bottom", crop_size.bottom, NULL);
    g_object_set(el_video->crop_v, "left", crop_size.left, NULL);
    g_object_set(el_video->crop_v, "right", crop_size.right, NULL);

    gst_element_set_state(el_video->crop_v, GST_STATE_PLAYING);

    return GST_PAD_PROBE_DROP;
}

GstPadProbeReturn
videopad_probe_cb(GstPad *blockpad, GstPadProbeInfo *info, gpointer user_data)
{
    GstPad *srcpad, *sinkpad;

    gst_pad_remove_probe(blockpad, GST_PAD_PROBE_INFO_ID(info));

    srcpad = gst_element_get_static_pad(el_video->crop_v, "src");
    gst_pad_add_probe(srcpad, (GstPadProbeType)(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
    		downstream__video_event_probe_cb, user_data, NULL);
    gst_object_unref(srcpad);

    sinkpad = gst_element_get_static_pad(el_video->crop_v, "sink");
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
    el = (Element_preview*)malloc(sizeof(Element_preview));
    if (el == NULL) {
        printf("Error: Failed to create elements.\n");
        return -1;
    }
    memset(el, 0, sizeof(Element_preview));

    pipeline = gst_pipeline_new("zoom_pipeline");
    assert (pipeline != NULL);

    el->src = gst_element_factory_make("icamerasrc", "mipi_src");
    el->src_filter = gst_element_factory_make("capsfilter", "src_filter");
    el->q1 = gst_element_factory_make("queue", "queue");
    el->crop = gst_element_factory_make("videocrop", "crop");
    el->crop_filter = gst_element_factory_make("capsfilter", "crop_filter");
    el->scale = gst_element_factory_make("videoscale", "scale");
    el->scale_filter = gst_element_factory_make("capsfilter", "scale_fitler");
    el->tee = gst_element_factory_make("tee", "tee");
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

//    el->blockpad = gst_element_get_static_pad(el->q1, "src");
    el->blockpad = gst_element_get_static_pad(el->tee, "src");
    el->queue_blockpad = gst_element_get_static_pad(el->q1, "sink");

    //add and link
    gst_bin_add_many(GST_BIN(pipeline), el->src, el->src_filter, el->q1, el->crop, el->crop_filter,
        el->scale, el->scale_filter, el->tee, el->convert, el->sink, NULL);
//    gst_element_link_many(el->src, el->src_filter, el->q1, el->crop, el->crop_filter,
//        el->scale, el->scale_filter, el->convert, el->sink, el->tee,  NULL);

	if (!gst_element_link_many(el->src, el->src_filter, el->tee, NULL)
		|| !gst_element_link_many(el->tee, el->q1, el->crop, el->crop_filter, el->scale, el->scale_filter, el->convert, el->sink, NULL)) {
		g_error("Failed to link elements");
		return -2;
	}

	gst_pad_link(el->blockpad, el->queue_blockpad);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
	gst_object_unref(el->queue_blockpad);

    printf("thread playing...\n");
    return 0;
}
/*
 *
 *sudo -E gst-launch-1.0 icamerasrc device-name=ov13858-uf af-mode=2 printfps=true name=t \
 *    t.src     ! video/x-raw,format=NV12,width=1280,height=720 ! videocrop top=30 left=50 right=50 bottom=30 ! videoscale ! video/x-raw,width=1280,height=720 ! videoconvert ! ximagesink \
 *    t.still_0 ! video/x-raw,format=NV12,width=4096,height=3072 ! videocrop top=128 left=160 right=160 bottom=128 ! videoscale ! video/x-raw,width=4096,height=3072  ! videoconvert ! ximagesink
 *
 */
GstPadProbeReturn
captureImage_probe_cb(GstPad *blockpad, GstPadProbeInfo *info, gpointer user_data)
{
    g_print("startCaptureImage\n");
     GstCaps *caps = NULL;
    gst_pad_remove_probe(blockpad, GST_PAD_PROBE_INFO_ID(info));
    g_print("startCaptureImage: %s : %d \n", __func__, __LINE__);
    GstPadTemplate *templ;
    
    el_image = (Element_image*)malloc(sizeof(Element_image));
    if (el_image == NULL) {
        printf("Error: Failed to create elements.\n");
        return GST_PAD_PROBE_DROP;
    }
    memset(el_image, 0, sizeof(Element_image));

    templ = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(el->tee), "src_%u");
    el_image->tee_imagepad = gst_element_request_pad(el->tee, templ, NULL, NULL);
    el_image->queue_image = gst_element_factory_make("queue", "queue_image");
    el_image->src_filter = gst_element_factory_make("capsfilter", "src_filter_i");
    el_image->crop = gst_element_factory_make("videocrop", "crop_i");
    el_image->crop_filter = gst_element_factory_make("capsfilter", "crop_filter_i");
    el_image->scale = gst_element_factory_make("videoscale", "scale_i");
    el_image->scale_filter = gst_element_factory_make("capsfilter", "scale_fitler_i");

    g_print("startCaptureImage: %s : %d \n", __func__, __LINE__);
    //Set icamerasrc caps.
    caps = gst_caps_new_simple("video/x-raw",
                "format", G_TYPE_STRING, "NV12",
                "width", G_TYPE_INT, 4096,
                "height", G_TYPE_INT, 3072,
                NULL);
    g_object_set(el_image->src_filter, "caps", caps, NULL);
    gst_caps_unref(caps);

    //set default crop: 0 0 0 0
    caps = gst_caps_new_simple("video/x-raw",
                "top", G_TYPE_INT, crop_image_size.top,
                "bottom", G_TYPE_INT, crop_image_size.bottom,
                "left", G_TYPE_INT, crop_image_size.left,
                "right", G_TYPE_INT, crop_image_size.right,
                NULL);
    g_object_set(el_image->crop_filter, "caps", caps, NULL);
    gst_caps_unref(caps);

    caps = gst_caps_new_simple("video/x-raw",
                "width", G_TYPE_INT, 4096,
                "height", G_TYPE_INT, 3072,
                NULL);
    g_object_set(el_image->scale_filter, "caps", caps, NULL);
    gst_caps_unref(caps);

    g_print("startCaptureImage: %s : %d \n", __func__, __LINE__);
//    el_image->appsink_convert = gst_element_factory_make ("videoconvert", "appsink_convert");
//    el_image->app_sink = gst_element_factory_make ("appsink", "app_sink");
//    gchar *appsink_caps_text = g_strdup_printf(CAPS);
//    el_image->appsink_caps = gst_caps_from_string(appsink_caps_text);
//    if(!el_image->appsink_caps)
//    {
//    	printf("Failed...\n");
//        return GST_PAD_PROBE_DROP;
//    }

    el_image->convert = gst_element_factory_make("videoconvert", "convert");
    el_image->sink = gst_element_factory_make("ximagesink", "imagesink");


    gst_bin_add_many(GST_BIN(pipeline), el_image->src_filter, el_image->queue_image, el_image->crop, el_image->crop_filter,
		el_image->scale, el_image->scale_filter, el_image->convert, el_image->sink, NULL);
    gst_element_link_many(el_image->src_filter, el_image->queue_image, el_image->crop, el_image->crop_filter,
		el_image->scale, el_image->scale_filter, el_image->convert, el_image->sink, NULL);
    g_print("startCaptureImage: %s : %d \n", __func__, __LINE__);
	gst_element_sync_state_with_parent(el_image->src_filter);
	gst_element_sync_state_with_parent(el_image->queue_image);
	gst_element_sync_state_with_parent(el_image->crop);
	gst_element_sync_state_with_parent(el_image->crop_filter);
	gst_element_sync_state_with_parent(el_image->scale);
	gst_element_sync_state_with_parent(el_image->scale_filter);
	gst_element_sync_state_with_parent(el_image->convert);
	gst_element_sync_state_with_parent(el_image->sink);

    g_print("startCaptureImage: %s : %d \n", __func__, __LINE__);
	el_image->queue_imagepad = gst_element_get_static_pad(el_image->queue_image, "sink");
	gst_pad_link(el_image->tee_imagepad, el_image->queue_imagepad);
	gst_object_unref(el_image->queue_imagepad);

    printf("thread playing...\n");

    return GST_PAD_PROBE_OK;
}

//sudo -E gst-launch-1.0 -v icamerasrc device-name=ov13858-uf af-mode=2 ! video/x-raw,format=NV12,width=640,height=480 \
	! videocrop top=35 left=320 right=4 bottom=20 ! videoscale ! video/x-raw,wdith=1920,height=1080 ! videoconvert ! jpegenc ! avimux ! filesink location=xyz.avi

//sudo -E gst-launch-1.0 icamerasrc device-name=ov13858-uf af-mode=2 printfps=true name=t \
    t.src     ! video/x-raw,format=NV12,width=1280,height=720 ! videocrop top=30 left=50 right=50 bottom=30 ! videoscale ! video/x-raw,width=1280,height=720 ! videoconvert ! ximagesink \
t.still_0 ! video/x-raw,format=NV12,width=1280,height=720 ! videocrop top=0 left=0 right=0 bottom=0 ! videoscale ! video/x-raw,wdith=1280,height=720 ! videoconvert ! jpegenc ! avimux ! filesink location=xyz.avi

GstPadProbeReturn
captureVideo_probe_cb(GstPad *blockpad, GstPadProbeInfo *info, gpointer user_data)
{
	g_print("startRecording\n");
	GstPadTemplate *templ;
     GstCaps *caps = NULL;
    gst_pad_remove_probe(blockpad, GST_PAD_PROBE_INFO_ID(info));

    el_video = (Element_video*)malloc(sizeof(Element_video));
    if (el_video == NULL) {
        printf("Error: Failed to create elements.\n");
        return GST_PAD_PROBE_DROP;
    }
    memset(el_video, 0, sizeof(Element_video));

    g_print("startCaptureVideo: %s : %d \n", __func__, __LINE__);

	templ = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(el->tee), "src_%u");
	el_video->tee_videopad = gst_element_request_pad(el->tee, templ, NULL, NULL);
	el_video->queue_video = gst_element_factory_make("queue", "queue_video");
	el_video->src_filter_v = gst_element_factory_make("capsfilter", "src_filter_v");
	el_video->crop_v = gst_element_factory_make("videocrop", "crop_v");
	el_video->crop_filter_v = gst_element_factory_make("capsfilter", "crop_filter_v");
	el_video->scale_v = gst_element_factory_make("videoscale", "scale_v");
	el_video->scale_filter_v = gst_element_factory_make("capsfilter", "scale_fitler_v");
	el_video->encoder = gst_element_factory_make("jpegenc", NULL);
	el_video->muxer = gst_element_factory_make("avimux", NULL);
	el_video->filesink = gst_element_factory_make("filesink", NULL);
	el_video->convert = gst_element_factory_make("videoconvert", "convert_v");
	char *file_name = (char*) malloc(255 * sizeof(char));
	sprintf(file_name, "%s%d.avi", file_path, counter++);
	g_print("Recording to file %s\n", file_name);
	g_object_set(el_video->filesink, "location", file_name, NULL);
//	g_object_set(el_video->encoder, "tune", 4, NULL);
	free(file_name);

    //Set icamerasrc caps.
    caps = gst_caps_new_simple("video/x-raw",
                "format", G_TYPE_STRING, "NV12",
                "width", G_TYPE_INT, 1280,
                "height", G_TYPE_INT, 720,
                NULL);
    g_object_set(el_video->src_filter_v, "caps", caps, NULL);
    gst_caps_unref(caps);

    //set default crop: 0 0 0 0
    caps = gst_caps_new_simple("video/x-raw",
                "top", G_TYPE_INT, crop_size.top,
                "bottom", G_TYPE_INT, crop_size.bottom,
                "left", G_TYPE_INT, crop_size.left,
                "right", G_TYPE_INT, crop_size.right,
                NULL);
    g_object_set(el_video->crop_filter_v, "caps", caps, NULL);
    gst_caps_unref(caps);

    caps = gst_caps_new_simple("video/x-raw",
                "width", G_TYPE_INT, 1280,
                "height", G_TYPE_INT, 720,
                NULL);
    g_object_set(el_video->scale_filter_v, "caps", caps, NULL);
    gst_caps_unref(caps);

    g_print("startCaptureVideo: %s : %d \n", __func__, __LINE__);

	gst_bin_add_many(GST_BIN(pipeline), el_video->src_filter_v, el_video->queue_video, el_video->crop_v,  el_video->crop_filter_v,
			el_video->scale_v, el_video->scale_filter_v, el_video->convert, el_video->encoder, el_video->muxer, el_video->filesink, NULL);
	gst_element_link_many(el_video->queue_video, el_video->src_filter_v, el_video->crop_v, el_video->crop_filter_v,
			el_video->scale_v, el_video->scale_filter_v, el_video->convert, el_video->encoder, el_video->muxer, el_video->filesink, NULL);

	gst_element_sync_state_with_parent(el_video->src_filter_v);
	gst_element_sync_state_with_parent(el_video->queue_video);
	gst_element_sync_state_with_parent(el_video->crop_v);
	gst_element_sync_state_with_parent(el_video->crop_filter_v);
	gst_element_sync_state_with_parent(el_video->scale_v);
	gst_element_sync_state_with_parent(el_video->scale_filter_v);
	gst_element_sync_state_with_parent(el_video->convert);
	gst_element_sync_state_with_parent(el_video->encoder);
	gst_element_sync_state_with_parent(el_video->muxer);
	gst_element_sync_state_with_parent(el_video->filesink);

	el_video->queue_videopad = gst_element_get_static_pad(el_video->queue_video, "sink");
	gst_pad_link(el_video->tee_videopad, el_video->queue_videopad);
	gst_object_unref(el_video->queue_videopad);

    printf("thread playing...\n");
	recording = TRUE;

    return GST_PAD_PROBE_OK;
}

void set_crop(void *args)
{
    gst_pad_add_probe(el->queue_blockpad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                    blockpad_probe_cb, (gpointer)args, NULL);

    return;
}

void set_crop_video(void *args)
{
    gst_pad_add_probe(el_video->tee_videopad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
    		videopad_probe_cb, (gpointer)args, NULL);

    return;
}

void take_picture(void *args)
{
    gst_pad_add_probe(el->queue_blockpad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                    captureImage_probe_cb, (gpointer)args, NULL);

    return;
}

void take_video(void *args)
{
    gst_pad_add_probe(el->queue_blockpad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                    captureVideo_probe_cb, (gpointer)args, NULL);

    return;
}

void* pipeline_thread_cb(void *args)
{
    create_pipeline(1280, 720);
    return NULL;
}

void sigintHandler(int unused) {
	g_print("You ctrl-c!\n");
	stopRecording();
}

int main(int argc, char *argv[])
{
    char c;
    gst_init(NULL, NULL);

    pthread_t pipeline_thread;
    pthread_create(&pipeline_thread, NULL, pipeline_thread_cb, NULL);
    printf("pause...\n");
    
    file_path = (char*) malloc(255 * sizeof(char));
    file_path = argv[1];
	signal(SIGINT, sigintHandler);

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
                crop_size.top += 30;
                crop_size.bottom += 30;
                crop_size.left += 50;
                crop_size.right += 50;
                crop_image_size.top += 128;
                crop_image_size.bottom += 128;
                crop_image_size.left += 160;
                crop_image_size.right += 160;
                set_crop(NULL);
                if (recording)
					set_crop_video(NULL);
            }
        } else if (c == 'd') {
            if (crop_size.top == 0) {
                printf("Min size, cannot zoom in\n");
            } else {
                crop_size.top -= 30;
                crop_size.bottom -= 30;
                crop_size.left -= 50;
                crop_size.right -= 50;
                crop_image_size.top -= 128;
                crop_image_size.bottom -= 128;
                crop_image_size.left -= 160;
                crop_image_size.right -= 160;
                set_crop(NULL);
                if (recording)
                    set_crop_video(NULL);
            }
        } else if (c == 'c') {
            take_picture(NULL);
        } else if (c == 'v') {
            take_video(NULL);
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
