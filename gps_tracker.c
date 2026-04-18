/**
 * GPS Tracker for Flipper Zero (Momentum Firmware)
 *
 * Hardware connection (GPIO header):
 *   Pin 13 (USART1_TX)  →  GPS RX
 *   Pin 14 (USART1_RX)  →  GPS TX
 *   Pin  9 (3.3 V)      →  GPS VCC  (3.3-V modules)
 *   Pin  1 (5 V)        →  GPS VCC  (5-V modules, e.g. NEO-6M breakout)
 *   Pin 11 (GND)        →  GPS GND
 *
 * Setup screen:
 *   UP / DOWN  – change baud rate
 *   OK         – confirm and start
 *   BACK       – exit app
 *
 * GPS screen:
 *   OK         – toggle SD logging
 *   BACK       – exit app
 *
 * Log file: /ext/gps_log.csv
 */

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <furi_hal_light.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_cdc.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <storage/storage.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ─────────────── Constants ─────────────── */

#define TAG             "GpsTracker"
#define GPS_SERIAL_ID   FuriHalSerialIdUsart
#define RX_STREAM_SIZE  512
#define NMEA_MAX_LEN    96
#define LOG_FILE_PATH   EXT_PATH("gps_log.csv")
#define LOG_INTERVAL_MS 1000

static const uint32_t BAUD_OPTIONS[] = {9600, 19200, 38400, 115200};
static const uint8_t  BAUD_COUNT     = 4;
static const uint8_t  BAUD_DEFAULT   = 0;   /* index → 9600 */

/* ─────────────── Fix quality levels ─────────────── */

typedef enum {
    GpsFixNone = 0,  /* no fix */
    GpsFix2D   = 1,  /* 2D fix */
    GpsFix3D   = 2,  /* 3D fix */
    GpsFixDGPS = 3,  /* Differential GPS */
    GpsFixRTK  = 4,  /* RTK fixed or float */
} GpsFixQuality;

/* ─────────────── Screen enum ─────────────── */

typedef enum {
    AppScreenSetup,
    AppScreenGps,
} AppScreen;

/* ─────────────── App state ─────────────── */

typedef struct {
    AppScreen screen;
    uint8_t   baud_idx;

    float    latitude;
    float    longitude;
    float    altitude_m;
    float    speed_kmh;
    uint8_t  hour, minute, second;
    uint8_t  day, month;
    uint16_t year;
    uint8_t  satellites;
    bool          fix_valid;
    bool          has_data;
    GpsFixQuality fix_quality;
    bool          new_epoch;  /* set by RMC, consumed by worker for LED */
    uint32_t log_entry_count;

    FuriMutex*        mutex;
    Gui*              gui;
    ViewPort*         view_port;
    FuriMessageQueue* event_queue;

    FuriHalSerialHandle* serial_handle;
    FuriStreamBuffer*    rx_stream;
    FuriThread*          worker_thread;
    volatile bool        worker_running;

    Storage* storage;
    File*    log_file;
    bool     logging;
    uint32_t last_log_tick;

    NotificationApp* notifications;

    /* Backlight + toast */
    bool     backlight_on;   /* true=enforce on, false=enforce off */
    bool     backlight_set;  /* has the user set a preference yet? */
    uint32_t toast_until;    /* show toast overlay until this tick */
    char     toast_msg[22];
} GpsApp;

/* ═══════════════════════════════════════════
   NMEA helpers
   ═══════════════════════════════════════════ */

static int nmea_split(char* line, char** fields, int max_fields) {
    int n = 0;
    fields[n++] = line;
    for(char* p = line; *p && n < max_fields; p++) {
        if(*p == ',') {
            *p = '\0';
            fields[n++] = p + 1;
        }
    }
    return n;
}

static float nmea_coord_to_decimal(const char* val, const char* dir) {
    if(!val || strlen(val) < 3) return 0.0f;
    float raw     = strtof(val, NULL);
    int   degrees = (int)(raw / 100.0f);
    float minutes = raw - (float)(degrees * 100);
    float result  = (float)degrees + minutes / 60.0f;
    if(dir && (dir[0] == 'S' || dir[0] == 'W')) result = -result;
    return result;
}

static void parse_u8_pair(const char* str, int offset, uint8_t* out) {
    if(!str || (int)strlen(str) < offset + 2) return;
    char tmp[3] = {str[offset], str[offset + 1], '\0'};
    *out = (uint8_t)strtol(tmp, NULL, 10);
}

static void nmea_parse_rmc(GpsApp* app, char* sentence) {
    char  buf[NMEA_MAX_LEN];
    strncpy(buf, sentence, NMEA_MAX_LEN - 1);
    buf[NMEA_MAX_LEN - 1] = '\0';

    char* f[14] = {0};
    int   n     = nmea_split(buf, f, 14);
    if(n < 10) return;

    if(f[1] && strlen(f[1]) >= 6) {
        parse_u8_pair(f[1], 0, &app->hour);
        parse_u8_pair(f[1], 2, &app->minute);
        parse_u8_pair(f[1], 4, &app->second);
    }
    app->fix_valid = f[2] && (f[2][0] == 'A');
    if(f[3] && f[4]) app->latitude  = nmea_coord_to_decimal(f[3], f[4]);
    if(f[5] && f[6]) app->longitude = nmea_coord_to_decimal(f[5], f[6]);
    if(f[7])         app->speed_kmh = strtof(f[7], NULL) * 1.852f;

    if(n > 9 && f[9] && strlen(f[9]) >= 6) {
        parse_u8_pair(f[9], 0, &app->day);
        parse_u8_pair(f[9], 2, &app->month);
        uint8_t yy = 0;
        parse_u8_pair(f[9], 4, &yy);
        app->year = 2000u + yy;
    }
    app->has_data  = true;
    app->new_epoch = true;
}

static void nmea_parse_gga(GpsApp* app, char* sentence) {
    char  buf[NMEA_MAX_LEN];
    strncpy(buf, sentence, NMEA_MAX_LEN - 1);
    buf[NMEA_MAX_LEN - 1] = '\0';

    char* f[15] = {0};
    int   n     = nmea_split(buf, f, 15);
    if(n < 10) return;

    if(f[7]) app->satellites = (uint8_t)strtol(f[7], NULL, 10);
    if(f[9]) app->altitude_m = strtof(f[9], NULL);
    if(f[6]) {
        uint8_t q = (uint8_t)strtol(f[6], NULL, 10);
        if(q == 0)        app->fix_quality = GpsFixNone;
        else if(q == 2)   app->fix_quality = GpsFixDGPS;
        else if(q >= 4)   app->fix_quality = GpsFixRTK;
        else /* q==1 */   app->fix_quality =
                              (app->satellites >= 4) ? GpsFix3D : GpsFix2D;
    }
}

static void nmea_dispatch(GpsApp* app, char* sentence) {
    if(strncmp(sentence, "$GPRMC", 6) == 0 ||
       strncmp(sentence, "$GNRMC", 6) == 0 ||
       strncmp(sentence, "$GCRMC", 6) == 0) {
        nmea_parse_rmc(app, sentence);
    } else if(strncmp(sentence, "$GPGGA", 6) == 0 ||
              strncmp(sentence, "$GNGGA", 6) == 0) {
        nmea_parse_gga(app, sentence);
    }
}

/* ═══════════════════════════════════════════
   Maidenhead locator (6 chars)
   ═══════════════════════════════════════════ */

static void latlon_to_maidenhead(float lat, float lon, char* out) {
    /* Normalize to positive range */
    float lo = lon + 180.0f;
    float la = lat +  90.0f;

    /* Field  A-R (20° / 10°) */
    out[0] = (char)('A' + (int)(lo / 20.0f));
    out[1] = (char)('A' + (int)(la / 10.0f));

    /* Square  0-9 (2° / 1°) */
    out[2] = (char)('0' + (int)(fmodf(lo, 20.0f) / 2.0f));
    out[3] = (char)('0' + (int)(fmodf(la, 10.0f) / 1.0f));

    /* Sub-square  a-x (5' / 2.5') */
    float ss_lo = fmodf(lo, 2.0f) * 12.0f;   /* 0..24 */
    float ss_la = fmodf(la, 1.0f) * 24.0f;   /* 0..24 */
    out[4] = (char)('a' + (int)(ss_lo));
    out[5] = (char)('a' + (int)(ss_la));

    /* Extended square  0-9 (30" / 15") */
    out[6] = (char)('0' + (int)(fmodf(ss_lo, 1.0f) * 10.0f));
    out[7] = (char)('0' + (int)(fmodf(ss_la, 1.0f) * 10.0f));

    out[8] = '\0';
}

/* ═══════════════════════════════════════════
   Epoch LED blink sequences
   ═══════════════════════════════════════════ */

/* No fix  : R · B · R */
static const NotificationSequence seq_epoch_no_fix = {
    &message_red_255,   &message_delay_50, &message_red_0,
    &message_blue_255,  &message_delay_50, &message_blue_0,
    &message_red_255,   &message_delay_50, &message_red_0,
    NULL,
};

/* 2D / 3D : G · B · G */
static const NotificationSequence seq_epoch_fix = {
    &message_green_255, &message_delay_50, &message_green_0,
    &message_blue_255,  &message_delay_50, &message_blue_0,
    &message_green_255, &message_delay_50, &message_green_0,
    NULL,
};

/* DGPS/RTK : G · G · G */
static const NotificationSequence seq_epoch_precise = {
    &message_green_255, &message_delay_50, &message_green_0,
    &message_green_255, &message_delay_50, &message_green_0,
    &message_green_255, &message_delay_50, &message_green_0,
    NULL,
};

/* ═══════════════════════════════════════════
   SD card logging
   ═══════════════════════════════════════════ */

static bool gps_log_start(GpsApp* app) {
    app->log_file    = storage_file_alloc(app->storage);
    bool file_exists = storage_common_exists(app->storage, LOG_FILE_PATH);

    if(!storage_file_open(app->log_file, LOG_FILE_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        FURI_LOG_E(TAG, "Cannot open log file");
        storage_file_free(app->log_file);
        app->log_file = NULL;
        return false;
    }
    if(!file_exists) {
        const char* hdr =
            "DateTime_UTC,Latitude,Longitude,Altitude_m,Speed_kmh,Satellites\n";
        storage_file_write(app->log_file, hdr, strlen(hdr));
    }
    app->log_entry_count = 0;
    app->last_log_tick   = 0;
    return true;
}

static void gps_log_stop(GpsApp* app) {
    if(app->log_file) {
        storage_file_close(app->log_file);
        storage_file_free(app->log_file);
        app->log_file = NULL;
    }
}

static void gps_log_write_locked(GpsApp* app) {
    if(!app->log_file || !app->fix_valid) return;
    uint32_t now = furi_get_tick();
    if((now - app->last_log_tick) < LOG_INTERVAL_MS) return;
    app->last_log_tick = now;

    char line[128];
    int  len = snprintf(
        line, sizeof(line),
        "%04u-%02u-%02uT%02u:%02u:%02uZ,%.6f,%.6f,%.1f,%.2f,%u\n",
        app->year, app->month, app->day,
        app->hour, app->minute, app->second,
        (double)app->latitude, (double)app->longitude,
        (double)app->altitude_m, (double)app->speed_kmh,
        app->satellites);

    if(len > 0 && len < (int)sizeof(line)) {
        storage_file_write(app->log_file, line, (uint16_t)len);
        app->log_entry_count++;
    }
}

/* ═══════════════════════════════════════════
   Serial
   ═══════════════════════════════════════════ */

static void gps_serial_rx_cb(
    FuriHalSerialHandle* handle,
    FuriHalSerialRxEvent event,
    void* ctx) {
    GpsApp* app = (GpsApp*)ctx;
    if(event == FuriHalSerialRxEventData) {
        uint8_t byte = furi_hal_serial_async_rx(handle);
        furi_stream_buffer_send(app->rx_stream, &byte, 1, 0);
    }
}

static void gps_serial_start(GpsApp* app) {
    app->serial_handle = furi_hal_serial_control_acquire(GPS_SERIAL_ID);
    furi_check(app->serial_handle);
    furi_hal_serial_init(app->serial_handle, BAUD_OPTIONS[app->baud_idx]);
    furi_hal_serial_async_rx_start(app->serial_handle, gps_serial_rx_cb, app, false);
}

static void gps_serial_stop(GpsApp* app) {
    if(app->serial_handle) {
        furi_hal_serial_async_rx_stop(app->serial_handle);
        furi_hal_serial_deinit(app->serial_handle);
        furi_hal_serial_control_release(app->serial_handle);
        app->serial_handle = NULL;
    }
}

/* ═══════════════════════════════════════════
   USB CDC forward (NMEA → PC)
   Safe to call at any time; silently skips if no host is connected.
   Uses interface 0 (main VCP). Sending only – does not interfere
   with GPS parsing, SD logging, or GUI in any way.
   ═══════════════════════════════════════════ */

static void gps_usb_forward(const char* sentence, size_t len) {
    /* Guard: skip if USB is not enumerated (no PC / cable unplugged) */
    if(!furi_hal_usb_get_config()) return;

    /* Build one contiguous buffer: sentence + CRLF, single send */
    if(len + 2 > NMEA_MAX_LEN + 2) return;  /* safety */
    char out[NMEA_MAX_LEN + 2];
    memcpy(out, sentence, len);
    out[len]     = '\r';
    out[len + 1] = '\n';
    furi_hal_cdc_send(0, (uint8_t*)out, (uint16_t)(len + 2));
}

/* ═══════════════════════════════════════════
   Worker thread
   ═══════════════════════════════════════════ */

static int32_t gps_worker_thread(void* ctx) {
    GpsApp*  app         = (GpsApp*)ctx;
    char     nmea_buf[NMEA_MAX_LEN];
    size_t   nmea_len    = 0;
    bool     in_sentence = false;
    uint8_t  byte;

    while(app->worker_running) {
        if(furi_stream_buffer_receive(app->rx_stream, &byte, 1, 50) == 0) continue;

        if(byte == '$') {
            in_sentence          = true;
            nmea_len             = 0;
            nmea_buf[nmea_len++] = '$';
        } else if(in_sentence) {
            if(byte == '\n' || byte == '\r') {
                if(nmea_len > 6) {
                    nmea_buf[nmea_len] = '\0';

                    /* Forward complete raw sentence (with checksum) to PC */
                    gps_usb_forward(nmea_buf, nmea_len);

                    /* Strip checksum for internal parsing */
                    char* star = strchr(nmea_buf, '*');
                    if(star) *star = '\0';

                    bool          do_led = false;
                    GpsFixQuality led_fq  = GpsFixNone;

                    furi_mutex_acquire(app->mutex, FuriWaitForever);
                    nmea_dispatch(app, nmea_buf);
                    if(app->logging && app->log_file) gps_log_write_locked(app);
                    if(app->new_epoch) {
                        do_led        = true;
                        led_fq        = app->fix_quality;
                        app->new_epoch = false;
                    }
                    furi_mutex_release(app->mutex);

                    if(do_led) {
                        const NotificationSequence* seq =
                            (led_fq >= GpsFixRTK)  ? &seq_epoch_precise :
                            (led_fq >= GpsFix2D)   ? &seq_epoch_fix     :
                                                      &seq_epoch_no_fix;
                        notification_message(app->notifications, seq);
                    }

                    view_port_update(app->view_port);
                }
                in_sentence = false;
                nmea_len    = 0;
            } else if(nmea_len < NMEA_MAX_LEN - 1) {
                nmea_buf[nmea_len++] = (char)byte;
            } else {
                in_sentence = false;
                nmea_len    = 0;
            }
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════
   Draw callback
   ═══════════════════════════════════════════ */

static void gps_draw_cb(Canvas* canvas, void* ctx) {
    GpsApp* app = (GpsApp*)ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    /* ─────────── SETUP SCREEN ─────────── */
    if(app->screen == AppScreenSetup) {

        /* Title */
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 10, "GPS Tracker Setup");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_line(canvas, 0, 12, 127, 12);

        /* Pin table (two columns) */
        canvas_draw_str(canvas, 2,  22, "TX  Pin 13  ->  GPS RX");
        canvas_draw_str(canvas, 2,  31, "RX  Pin 14  <-  GPS TX");
        canvas_draw_str(canvas, 2,  40, "VCC Pin  9 (3.3V) / 1 (5V)");
        canvas_draw_str(canvas, 2,  49, "GND Pin 11");

        canvas_draw_line(canvas, 0, 51, 127, 51);

        /* Baud rate row */
        canvas_draw_str(canvas, 2, 62, "Baud:");

        /* Four baud labels at fixed x, selected one inverted */
        const uint8_t col_x[4] = {33, 53, 79, 104};
        const char*   labels[4] = {"9k6", "19k2", "38k4", "115k"};

        for(uint8_t i = 0; i < BAUD_COUNT; i++) {
            uint8_t lw = (uint8_t)(strlen(labels[i]) * 5 + 2);
            if(i == app->baud_idx) {
                canvas_draw_box(canvas, col_x[i] - 1, 53, lw, 10);
                canvas_invert_color(canvas);
                canvas_draw_str(canvas, col_x[i], 62, labels[i]);
                canvas_invert_color(canvas);
            } else {
                canvas_draw_str(canvas, col_x[i], 62, labels[i]);
            }
        }

    /* ─────────── GPS SCREEN ─────────── */
    } else {

        /* Title bar */
        canvas_draw_str(canvas, 0, 8, "GPS Tracker");

        if(app->logging) {
            char lbuf[14];
            snprintf(lbuf, sizeof(lbuf), "LOG*%lu",
                     (unsigned long)app->log_entry_count);
            canvas_draw_str(canvas, 79, 8, lbuf);
        } else {
            canvas_draw_str(canvas, 88, 8, "LOG OFF");
        }
        canvas_draw_line(canvas, 0, 10, 127, 10);

        char buf[32];

        if(!app->has_data) {
            char bstr[20];
            snprintf(bstr, sizeof(bstr), "@ %lu baud",
                     (unsigned long)BAUD_OPTIONS[app->baud_idx]);
            canvas_draw_str(canvas, 4, 28, "Searching for GPS...");
            canvas_draw_str(canvas, 24, 38, bstr);
            canvas_draw_str(canvas, 16, 52, "Check wiring");
        } else {
            char mh[9];
            latlon_to_maidenhead(app->latitude, app->longitude, mh);

            snprintf(buf, sizeof(buf), "Lat:%11.7f %c",
                     (double)fabsf(app->latitude),
                     app->latitude >= 0.0f ? 'N' : 'S');
            canvas_draw_str(canvas, 0, 21, buf);

            snprintf(buf, sizeof(buf), "Lon:%11.7f %c",
                     (double)fabsf(app->longitude),
                     app->longitude >= 0.0f ? 'E' : 'W');
            canvas_draw_str(canvas, 0, 31, buf);

            snprintf(buf, sizeof(buf), "Alt:%6.0fm  Sat:%02u",
                     (double)app->altitude_m, app->satellites);
            canvas_draw_str(canvas, 0, 41, buf);

            /* Speed left, Maidenhead right on same line */
            snprintf(buf, sizeof(buf), "Spd:%5.1fkm/h",
                     (double)app->speed_kmh);
            canvas_draw_str(canvas, 0, 51, buf);
            /* Right-align MH: FontSecondary ~6 px/char incl. spacing */
            uint8_t mh_x = (uint8_t)(128 - strlen(mh) * 6);
            canvas_draw_str(canvas, mh_x, 51, mh);

            /* Fix status at fixed x=64 (screen centre) */
            static const char* const fix_labels[] = {
                "[----]", "[ 2D ]", "[ 3D ]", "[DGPS]", "[ RTK]"
            };
            uint8_t fqi = (uint8_t)app->fix_quality;
            if(fqi > 4) fqi = 0;
            const char* fix_str = fix_labels[fqi];
            snprintf(buf, sizeof(buf), "%02u:%02u:%02uZ",
                     app->hour, app->minute, app->second);
            canvas_draw_str(canvas, 0, 62, buf);
            canvas_draw_str(canvas, 64, 62, fix_str);

            /* Toast overlay (backlight feedback) */
            if(furi_get_tick() < app->toast_until) {
                uint8_t tw = (uint8_t)(strlen(app->toast_msg) * 5 + 4);
                uint8_t tx = (128 - tw) / 2;
                canvas_draw_box(canvas, tx - 1, 24, tw + 2, 12);
                canvas_invert_color(canvas);
                canvas_draw_str(canvas, tx + 1, 34, app->toast_msg);
                canvas_invert_color(canvas);
            }
        }
    }

    furi_mutex_release(app->mutex);
}

/* ═══════════════════════════════════════════
   Input callback
   ═══════════════════════════════════════════ */

static void gps_input_cb(InputEvent* event, void* ctx) {
    GpsApp* app = (GpsApp*)ctx;
    furi_message_queue_put(app->event_queue, event, FuriWaitForever);
}

/* ═══════════════════════════════════════════
   Entry point
   ═══════════════════════════════════════════ */

int32_t gps_tracker_app(void* p) {
    UNUSED(p);

    GpsApp* app = malloc(sizeof(GpsApp));
    furi_check(app);
    memset(app, 0, sizeof(GpsApp));

    app->screen   = AppScreenSetup;
    app->baud_idx = BAUD_DEFAULT;

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    furi_check(app->mutex);

    app->event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    furi_check(app->event_queue);

    app->rx_stream = furi_stream_buffer_alloc(RX_STREAM_SIZE, 1);
    furi_check(app->rx_stream);

    app->view_port = view_port_alloc();
    furi_check(app->view_port);
    view_port_draw_callback_set(app->view_port, gps_draw_cb, app);
    view_port_input_callback_set(app->view_port, gps_input_cb, app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    app->storage       = furi_record_open(RECORD_STORAGE);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    view_port_update(app->view_port);

    /* ════════════════════════
       Event loop
       ════════════════════════ */
    InputEvent event;
    bool       running = true;

    while(running) {
        if(furi_message_queue_get(app->event_queue, &event, 200) != FuriStatusOk) continue;
        if(event.type != InputTypeShort) continue;

        /* ── Setup screen ── */
        if(app->screen == AppScreenSetup) {
            switch(event.key) {

            case InputKeyBack:
                running = false;
                break;

            case InputKeyLeft:
            case InputKeyUp:
                if(app->baud_idx > 0) {
                    app->baud_idx--;
                    view_port_update(app->view_port);
                }
                break;

            case InputKeyRight:
            case InputKeyDown:
                if(app->baud_idx < BAUD_COUNT - 1) {
                    app->baud_idx++;
                    view_port_update(app->view_port);
                }
                break;

            case InputKeyOk:
                gps_serial_start(app);

                app->worker_running = true;
                app->worker_thread  = furi_thread_alloc();
                furi_thread_set_name(app->worker_thread, "GpsWorker");
                furi_thread_set_stack_size(app->worker_thread, 2048);
                furi_thread_set_callback(app->worker_thread, gps_worker_thread);
                furi_thread_set_context(app->worker_thread, app);
                furi_thread_start(app->worker_thread);

                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->screen = AppScreenGps;
                furi_mutex_release(app->mutex);

                notification_message(app->notifications, &sequence_blink_blue_10);
                view_port_update(app->view_port);
                break;

            default:
                break;
            }

        /* ── GPS screen ── */
        } else {
            switch(event.key) {

            case InputKeyBack:
                running = false;
                break;

            case InputKeyUp:
                /* Backlight permanently ON */
                notification_message(app->notifications,
                    &sequence_display_backlight_enforce_on);
                app->backlight_on  = true;
                app->backlight_set = true;
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                strncpy(app->toast_msg, "Licht: dauerhaft AN", sizeof(app->toast_msg) - 1);
                app->toast_until = furi_get_tick() + 1800;
                furi_mutex_release(app->mutex);
                view_port_update(app->view_port);
                break;

            case InputKeyDown:
                /* Backlight permanently OFF:
                   switch to auto mode first, then force level to 0 */
                notification_message(app->notifications,
                    &sequence_display_backlight_enforce_auto);
                furi_hal_light_set(LightBacklight, 0);
                app->backlight_on  = false;
                app->backlight_set = true;
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                strncpy(app->toast_msg, "Licht: dauerhaft AUS", sizeof(app->toast_msg) - 1);
                app->toast_until = furi_get_tick() + 1800;
                furi_mutex_release(app->mutex);
                view_port_update(app->view_port);
                break;

            case InputKeyOk:
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                if(app->logging) {
                    app->logging = false;
                    gps_log_stop(app);
                    furi_mutex_release(app->mutex);
                    notification_message(app->notifications, &sequence_error);
                } else {
                    if(gps_log_start(app)) {
                        app->logging = true;
                        furi_mutex_release(app->mutex);
                        notification_message(app->notifications, &sequence_success);
                    } else {
                        furi_mutex_release(app->mutex);
                        notification_message(app->notifications, &sequence_error);
                    }
                }
                view_port_update(app->view_port);
                break;

            default:
                break;
            }
        }
    }

    /* ════════════════════════
       Cleanup
       ════════════════════════ */

    if(app->worker_thread) {
        app->worker_running = false;
        furi_thread_join(app->worker_thread);
        furi_thread_free(app->worker_thread);
    }

    /* Restore backlight to automatic mode */
    notification_message(app->notifications, &sequence_display_backlight_enforce_auto);

    gps_serial_stop(app);

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    if(app->logging) {
        app->logging = false;
        gps_log_stop(app);
    }
    furi_mutex_release(app->mutex);

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);

    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);

    furi_stream_buffer_free(app->rx_stream);
    furi_message_queue_free(app->event_queue);
    furi_mutex_free(app->mutex);

    free(app);
    FURI_LOG_I(TAG, "App exited cleanly");
    return 0;
}
