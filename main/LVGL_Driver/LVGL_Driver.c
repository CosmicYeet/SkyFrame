#include "LVGL_Driver.h"

static const char *LVGL_TAG = "LVGL";   
lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
lv_disp_drv_t disp_drv;      // contains callback functions

lv_indev_drv_t indev_drv;
esp_timer_handle_t lvgl_tick_timer = NULL;


static void *buf1 = NULL;
#if CONFIG_EXAMPLE_DOUBLE_FB
static void *buf2 = NULL;
#endif


/* Frame timing (UI thread only): see lvgl_frame_stats_t. */
static lvgl_frame_stats_t frame_stats;
static int64_t frame_t0, frame_wait;   /* frame_t0 = 0: this frame's start isn't known */
void lvgl_render_begin(void) { frame_t0 = esp_timer_get_time(); frame_wait = 0; }
void lvgl_frame_stats_take(lvgl_frame_stats_t *out) { *out = frame_stats; memset(&frame_stats, 0, sizeof(frame_stats)); }

void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
#if CONFIG_EXAMPLE_AVOID_TEAR_EFFECT_WITH_SEM
    /* Wait for vsync once per frame, before its first area. A frame is often several
       small areas; waiting before each one cost a whole panel refresh per area. */
    static bool in_frame;
    if (!in_frame) {
        int64_t w0 = esp_timer_get_time();
        xSemaphoreGive(sem_gui_ready);
        xSemaphoreTake(sem_vsync_end, portMAX_DELAY);
        int64_t w = esp_timer_get_time() - w0;
        frame_wait += w;
        frame_stats.wait_us += (uint64_t)w;
        in_frame = true;
    }
#endif
    // pass the draw buffer to the driver
    int64_t c0 = esp_timer_get_time();
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
    frame_stats.copy_us += (uint64_t)(esp_timer_get_time() - c0);
    frame_stats.areas++;
    frame_stats.px += (uint32_t)((offsetx2 - offsetx1 + 1) * (offsety2 - offsety1 + 1));
#if CONFIG_EXAMPLE_AVOID_TEAR_EFFECT_WITH_SEM
    if (lv_disp_flush_is_last(drv)) in_frame = false;
#endif
    if (lv_disp_flush_is_last(drv)) {
        int64_t now = esp_timer_get_time();
        frame_stats.frames++;
        if (frame_t0) {
            int64_t work = now - frame_t0 - frame_wait;
            if (work < 0) work = 0;
            frame_stats.measured++;
            frame_stats.work_us += (uint64_t)work;
            if (work > frame_stats.work_max_us) frame_stats.work_max_us = (uint32_t)work;
        }
        frame_t0 = now;   /* another frame in the same pass starts here */
        frame_wait = 0;
    }
    lv_disp_flush_ready(drv);
}

void example_increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

/*Read the touchpad*/
void example_touchpad_read( lv_indev_drv_t * drv, lv_indev_data_t * data )
{
    uint16_t touchpad_x[1] = {0};
    uint16_t touchpad_y[1] = {0};
    uint8_t touchpad_cnt = 0;

    /* Read touch controller data */
    esp_lcd_touch_read_data(drv->user_data);

    /* Get coordinates */
    bool touchpad_pressed = esp_lcd_touch_get_coordinates(drv->user_data, touchpad_x, touchpad_y, NULL, &touchpad_cnt, 1);

    if (touchpad_pressed && touchpad_cnt > 0) {
        data->point.x = touchpad_x[0];
        data->point.y = touchpad_y[0];
        data->state = LV_INDEV_STATE_PR;
        // ESP_LOGI(LVGL_TAG, "X=%u Y=%u", data->point.x, data->point.y);
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}
const void *LCD_Frame_Buffer(void)
{
    void *fb = NULL;
    return esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 1, &fb) == ESP_OK ? fb : NULL;
}
void LVGL_Init(void)
{
    ESP_LOGI(LVGL_TAG, "Initialize LVGL library");
    lv_init();
#if CONFIG_EXAMPLE_DOUBLE_FB
    ESP_LOGI(LVGL_TAG, "Use frame buffers as LVGL draw buffers");
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &buf1, &buf2));
    // initialize LVGL draw buffers
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES);
#else
    ESP_LOGI(LVGL_TAG, "Allocate separate LVGL draw buffers from PSRAM");
    // Draw buffer size is given to LVGL in pixels, so allocate pixels * bytes-per-pixel.
    // One full-screen buffer: the flush copies synchronously (and waits for vsync), so a
    // second buffer could never be drawn into while the first is sent. Saves ~450 KB PSRAM.
    buf1 = heap_caps_malloc(EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    assert(buf1);
    lv_disp_draw_buf_init(&disp_buf, buf1, NULL, EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES);
#endif // CONFIG_EXAMPLE_DOUBLE_FB

    ESP_LOGI(LVGL_TAG, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = EXAMPLE_LCD_H_RES;
    disp_drv.ver_res = EXAMPLE_LCD_V_RES;
    disp_drv.flush_cb = example_lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
#if CONFIG_EXAMPLE_DOUBLE_FB
    disp_drv.full_refresh = true; // the full_refresh mode can maintain the synchronization between the two frame buffers
#endif
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

    ESP_LOGI(LVGL_TAG, "Install LVGL tick timer");
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &example_increase_lvgl_tick,
        .name = "lvgl_tick"
    };

    /********************* LVGL *********************/
    ESP_LOGI(LVGL_TAG,"Register display indev to LVGL");
    lv_indev_drv_init ( &indev_drv );
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.disp = disp;
    indev_drv.read_cb = example_touchpad_read;
    indev_drv.user_data = tp;
    lv_indev_drv_register( &indev_drv );

    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

}