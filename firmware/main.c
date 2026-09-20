// HTN 2026 badge — PRECISION FINDER proof of concept.
//
// Finds a chosen WiFi AP by signal. Works with ONE badge because the target is
// an access point, not a second badge. If the AP advertises 802.11mc FTM we
// also get a real time-of-flight distance in centimetres.
//
// What this CANNOT do, and why: no UWB and no magnetometer, so there is no
// direction arrow. Direction is replaced by a motion-gated warmer/colder
// gradient -- the honest substitute.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "led_strip.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_now.h"
#include <math.h>
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_timer.h"
#include <math.h>
#include "esp_mac.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "esp_now.h"
#include <math.h>
#include "freertos/semphr.h"
#include "font5x7.h"
#include "esp_heap_caps.h"

#define LCD_MOSI 10
#define LCD_CLK   1
#define LCD_CS    2
#define LCD_DC    0
#define LCD_RST   4
#define I2C_SDA   5
#define I2C_SCL   6
#define HC165_DATA 7
#define HC165_LOAD 20
#define HC165_CLK  21
#define PIN_START  9
#define LED_DIN    3
#define W 320
#define H 240
#define STRIPE 48                       // 5 stripes; 320*48*2 = 30 KB
#define ACCEL_ADDR 0x19
#define NOW_CHAN 1              // pinned so a sniffer knows which channel to watch

// ST7789 takes RGB565 MSB-first; a little-endian uint16 goes out byte-reversed,
// so pre-swap every colour at construction. Without this, white renders cyan.
#define RGB(r,g,b) ((uint16_t)__builtin_bswap16((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3))))
static const uint16_t C_BG=RGB(8,10,20), C_FG=RGB(235,238,245), C_DIM=RGB(120,130,150),
       C_ACC=RGB(255,110,60), C_OK=RGB(80,220,140), C_BAR=RGB(60,90,200), C_HDR=RGB(24,28,48);

enum { BTN_A, BTN_B, BTN_HOME, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_UP, BTN_AUX1 };
static const char *BTN_NAME[8]={"A","B","Home","Down","Left","Right","Up","Aux1"};

/* ---------------- gfx: stripe renderer ---------------- */
static esp_lcd_panel_handle_t panel;
static uint16_t *fb;                    // one stripe
static int fb_y0;                       // stripe top in screen coords
static SemaphoreHandle_t blit_done;     // esp_lcd blits asynchronously

static bool IRAM_ATTR on_blit_done(esp_lcd_panel_io_handle_t io,
                                   esp_lcd_panel_io_event_data_t *e, void *ctx){
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(blit_done, &hp);
    return hp == pdTRUE;
}

static inline void px(int x,int y,uint16_t c){
    int ly=y-fb_y0;
    if(x<0||x>=W||ly<0||ly>=STRIPE) return;
    fb[ly*W+x]=c;
}
static void rect(int x,int y,int w,int h,uint16_t c){
    for(int j=0;j<h;j++) for(int i=0;i<w;i++) px(x+i,y+j,c);
}
static void ch(int x,int y,char c,uint16_t col,int s){
    if(c<0x20||c>0x7f) c='?';
    const uint8_t *g=FONT5X7[c-0x20];
    for(int cx=0;cx<5;cx++){
        uint8_t bits=g[cx];
        for(int cy=0;cy<7;cy++) if(bits&(1<<cy)) rect(x+cx*s,y+cy*s,s,s,col);
    }
}
static void thick_line(int x0,int y0,int x1,int y1,int w,uint16_t c){
    int dx=abs(x1-x0), dy=abs(y1-y0);
    int n = (dx>dy?dx:dy); if(n<1) n=1;
    for(int i=0;i<=n;i++)
        rect(x0+(x1-x0)*i/n - w/2, y0+(y1-y0)*i/n - w/2, w, w, c);
}
static void text(int x,int y,const char*s,uint16_t col,int sc){
    for(;*s;s++){ ch(x,y,*s,col,sc); x+=6*sc; }
}
static int textw(const char*s,int sc){ return (int)strlen(s)*6*sc; }

static void (*page_draw)(void);
static void flush(void){
    for(fb_y0=0; fb_y0<H; fb_y0+=STRIPE){
        for(int i=0;i<W*STRIPE;i++) fb[i]=C_BG;
        page_draw();
        esp_lcd_panel_draw_bitmap(panel,0,fb_y0,W,fb_y0+STRIPE,fb);
        // MUST block here: the buffer is shared across stripes, and reusing it
        // while the transfer is still in flight paints the wrong band.
        xSemaphoreTake(blit_done, pdMS_TO_TICKS(200));
    }
}

/* ---------------- hardware ---------------- */
static void buttons_init(void){
    gpio_config_t o={.pin_bit_mask=(1ULL<<HC165_LOAD)|(1ULL<<HC165_CLK),.mode=GPIO_MODE_OUTPUT};
    gpio_config(&o);
    gpio_config_t i={.pin_bit_mask=(1ULL<<HC165_DATA)|(1ULL<<PIN_START),
                     .mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE};
    gpio_config(&i);
    gpio_set_level(HC165_LOAD,1); gpio_set_level(HC165_CLK,0);
}
static uint8_t buttons(void){
    gpio_set_level(HC165_LOAD,0); esp_rom_delay_us(5);
    gpio_set_level(HC165_LOAD,1); esp_rom_delay_us(5);
    uint8_t v=0;
    for(int i=0;i<8;i++){
        if(!gpio_get_level(HC165_DATA)) v|=(1u<<i);
        gpio_set_level(HC165_CLK,1); esp_rom_delay_us(5);
        gpio_set_level(HC165_CLK,0); esp_rom_delay_us(5);
    }
    return v;
}
static bool startbtn(void){ return gpio_get_level(PIN_START)==0; }

static void display_init(void){
    spi_bus_config_t b={.mosi_io_num=LCD_MOSI,.sclk_io_num=LCD_CLK,.miso_io_num=-1,
        .quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=W*STRIPE*2};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST,&b,SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t ic={.dc_gpio_num=LCD_DC,.cs_gpio_num=LCD_CS,
        .pclk_hz=40*1000*1000,.spi_mode=0,.trans_queue_depth=1,
        .lcd_cmd_bits=8,.lcd_param_bits=8};
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,&ic,&io));
    blit_done = xSemaphoreCreateBinary();
    esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = on_blit_done };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io,&cbs,NULL));
    esp_lcd_panel_dev_config_t pc={.reset_gpio_num=LCD_RST,
        .rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB,.bits_per_pixel=16};
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io,&pc,&panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel,true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel,true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel,true,false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel,true));
}

static i2c_master_bus_handle_t bus; static i2c_master_dev_handle_t accel;
static bool accel_ok=false;
static bool accel_init(void){
    i2c_master_bus_config_t bc={.i2c_port=-1,.sda_io_num=I2C_SDA,.scl_io_num=I2C_SCL,
        .clk_source=I2C_CLK_SRC_DEFAULT,.glitch_ignore_cnt=7,.flags.enable_internal_pullup=true};
    if(i2c_new_master_bus(&bc,&bus)!=ESP_OK) return false;
    i2c_device_config_t dc={.dev_addr_length=I2C_ADDR_BIT_LEN_7,
        .device_address=ACCEL_ADDR,.scl_speed_hz=400000};
    if(i2c_master_bus_add_device(bus,&dc,&accel)!=ESP_OK) return false;
    uint8_t reg=0x0F,who=0;
    if(i2c_master_transmit_receive(accel,&reg,1,&who,1,200)!=ESP_OK||who!=0x11) return false;
    uint8_t a[2]={0x20,0x57}; i2c_master_transmit(accel,a,2,200);
    uint8_t c[2]={0x23,0x80}; i2c_master_transmit(accel,c,2,200);
    return true;
}
static int ax,ay,az;
// The panel init mirrors X (esp_lcd_panel_mirror(true,false)), so the
// accelerometer's +X runs opposite to screen +X. Map through here for
// anything positional; ax/ay/az stay raw sensor values.
static inline int tilt_sx(void){ return -ax; }
static void accel_read(void){
    if(!accel_ok){ax=ay=az=0;return;}
    uint8_t r=0x28|0x80,d[6]={0};
    i2c_master_transmit_receive(accel,&r,1,d,6,200);
    ax=(int16_t)((d[1]<<8)|d[0])>>4; ay=(int16_t)((d[3]<<8)|d[2])>>4; az=(int16_t)((d[5]<<8)|d[4])>>4;
}

static led_strip_handle_t strip;
static void leds_init(void){
    led_strip_config_t s={.strip_gpio_num=LED_DIN,.max_leds=6,
        .led_model=LED_MODEL_WS2812,.led_pixel_format=LED_PIXEL_FORMAT_GRB};
    led_strip_rmt_config_t r={.clk_src=RMT_CLK_SRC_DEFAULT,.resolution_hz=10*1000*1000};
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&s,&r,&strip));
    led_strip_clear(strip);
}


/* ---------------- target tracking ---------------- */
#define MAXAP 16
static wifi_ap_record_t aps[MAXAP];
static uint16_t nap = 0;
static int sel = 0;            // cursor in the target list
static bool have_target = false;
static wifi_ap_record_t target;

// RSSI at 1 m and path-loss exponent. n=2.0 free space, 2.7-3.5 indoors.
#define RSSI_AT_1M   (-40.0f)
#define PATHLOSS_N    (2.7f)

static volatile int  rssi_raw = -100;
static volatile int  rssi_pkts = 0;
static float rssi_f = -100.0f;      // EMA
static float rssi_slow = -100.0f;   // slower EMA, for the gradient
static bool  locked_on = false;

#define HIST 96
static int8_t hist[HIST];
static int hist_w = 0;

static volatile uint32_t ftm_cm = 0;
static volatile int ftm_state = 0;   // 0 idle, 1 in progress, 2 ok, 3 fail
static bool target_has_ftm = false;

static float rssi_to_m(float r){
    return powf(10.0f, (RSSI_AT_1M - r) / (10.0f * PATHLOSS_N));
}

// Promiscuous sniffing gives ~10 beacons/sec from the target, vs ~1 scan every
// 2 s with active scanning. Much better gradient resolution.
static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type){
    if(type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *p = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t *f = p->payload;
    if((f[0] & 0xFC) != 0x80) return;              // beacon only
    if(memcmp(f + 10, target.bssid, 6) != 0) return; // addr2 == target
    rssi_raw = p->rx_ctrl.rssi;
    rssi_pkts++;
}

// Every badge is BOTH a beacon and a seeker: it runs a SoftAP with the
// 802.11mc FTM responder enabled, so the other badge can range to it, while
// also scanning and ranging itself. Symmetric - either can find the other.
#define FIND_CHAN 1
static char my_ssid[20];

/* ---- ESP-NOW ranging: no 102 ms beacon floor, so we can sample at 50 Hz ---- */
static uint8_t BCAST[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
static volatile int  now_rssi = -100;
static volatile int  now_pkts = 0;
static volatile int64_t now_last_us = 0;
static uint8_t peer_mac[6];
static bool peer_known = false;

static void now_rx(const esp_now_recv_info_t *i, const uint8_t *d, int len){
    if(peer_known && memcmp(i->src_addr, peer_mac, 6) != 0) return;
    if(i->rx_ctrl) now_rssi = i->rx_ctrl->rssi;
    now_pkts++;
    now_last_us = esp_timer_get_time();
}
static void espnow_up(void){
    if(esp_now_init() != ESP_OK) return;
    esp_now_register_recv_cb(now_rx);
    esp_now_peer_info_t p = { .channel = 0, .ifidx = WIFI_IF_AP, .encrypt = false };
    memcpy(p.peer_addr, BCAST, 6);
    esp_now_add_peer(&p);
}
static inline bool now_fresh(void){          // ESP-NOW data younger than 300 ms
    return now_last_us && (esp_timer_get_time() - now_last_us) < 300000;
}

static void wifi_up(void){
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t c = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&c));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    uint8_t m[6]; esp_read_mac(m, ESP_MAC_WIFI_SOFTAP);
    snprintf(my_ssid, sizeof my_ssid, "HTN-FIND-%02X%02X", m[4], m[5]);

    wifi_config_t ap = {0};
    strncpy((char*)ap.ap.ssid, my_ssid, sizeof ap.ap.ssid);
    ap.ap.ssid_len       = strlen(my_ssid);
    ap.ap.channel        = FIND_CHAN;
    ap.ap.authmode       = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 2;
    ap.ap.beacon_interval= 100;      // 10 beacons/sec -> good RSSI rate
    ap.ap.ftm_responder  = true;     // <- this is what enables ranging
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    espnow_up();
    printf("\nI am beaconing as %s on ch%d (FTM responder + ESP-NOW)\n", my_ssid, FIND_CHAN);
}
static void do_scan(void){
    esp_wifi_set_promiscuous(false);
    if(esp_wifi_scan_start(NULL, true) == ESP_OK){
        uint16_t n = MAXAP; nap = 0;
        if(esp_wifi_scan_get_ap_records(&n, aps) == ESP_OK) nap = n;
    }
    // put other badges first - they are what you actually want to find
    for(int i=0;i<nap;i++){
        if(strncmp((char*)aps[i].ssid,"HTN-FIND-",9)==0 &&
           strcmp((char*)aps[i].ssid,my_ssid)!=0){
            wifi_ap_record_t t=aps[0]; aps[0]=aps[i]; aps[i]=t; break;
        }
    }
    if(sel >= nap) sel = nap ? nap-1 : 0;
    int ftm_ap = 0;
    for(int i=0;i<nap;i++) if(aps[i].ftm_responder) ftm_ap++;
    (void)ftm_ap;
}
static void lock_target(void){
    target = aps[sel];
    target_has_ftm = target.ftm_responder;
    have_target = true; locked_on = false;
    rssi_f = rssi_slow = target.rssi;
    for(int i=0;i<HIST;i++) hist[i] = target.rssi;
    ftm_state = 0; ftm_cm = 0;
    // park on the target's channel and sniff its beacons
    memcpy(peer_mac, target.bssid, 6);
    peer_known = (strncmp((char*)target.ssid,"HTN-FIND-",9)==0);
    now_last_us = 0;
    esp_wifi_set_channel(target.primary, WIFI_SECOND_CHAN_NONE);
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(promisc_cb);
    esp_wifi_set_promiscuous(true);
}
static void ftm_evt(void *arg, esp_event_base_t base, int32_t id, void *data){
    if(id != WIFI_EVENT_FTM_REPORT) return;
    wifi_event_ftm_report_t *r = (wifi_event_ftm_report_t*)data;
    if(r->status == FTM_STATUS_SUCCESS){
        // Multipath can only ever make the estimate LONGER, never shorter, and
        // a human cannot close 1.5 m/s. Reject physically impossible jumps.
        uint32_t prev = ftm_cm;
        if(prev == 0 || (uint32_t)abs((int)r->dist_est - (int)prev) < 400)
            ftm_cm = r->dist_est;
        else if(r->dist_est < prev) ftm_cm = r->dist_est;   // shorter = likelier LOS
        ftm_state = 2;
    }
    else ftm_state = 3;
}
static bool ftm_auto = true;
static void ftm_go(void){
    if(!have_target || !target_has_ftm) return;
    if(ftm_state == 1) return;                 // one session at a time
    // 8 frames instead of 16: roughly half the airtime, so we can repeat it
    // continuously and keep a live distance rather than a press-to-measure one.
    wifi_ftm_initiator_cfg_t cfg = { .frm_count = 8, .burst_period = 2 };
    memcpy(cfg.resp_mac, target.bssid, 6);
    cfg.channel = target.primary;
    ftm_state = (esp_wifi_ftm_initiate_session(&cfg) == ESP_OK) ? 1 : 3;
}

/* ---- LIVE COMPASS: heading-free angle from closing rate ----
   There is no gyro or magnetometer, so after the spin we cannot know which way
   you are now facing. But we do not need to. Walking gives us the angle
   directly: if you move a distance S and the range to the target changes by D,
   then the angle between your heading and the target is

        theta = acos( -D / S )

   straight at it -> D = -S -> 0 deg ; perpendicular -> D = 0 -> 90 deg ;
   directly away  -> D = +S -> 180 deg.

   Sign (left vs right) is not observable this way, so we take it from the
   calibration spin and keep it until a new spin says otherwise. */
static float dist_m = -1;            // fused range estimate, metres
static int   steps = 0;
static float walked = 0;             // metres since the window opened
static float win_d0 = -1;            // range when this window opened
static float live_angle = -1;        // deg off current heading
static int   live_side = +1;         // +1 = target is to the right
static bool  live_valid = false;
#define STEP_M 0.72f                 // average stride

static void step_detected(void){
    steps++;
    walked += STEP_M;
    if(win_d0 < 0) win_d0 = dist_m;
    // Recompute over ~4 strides: long enough that the range change clears the
    // FTM noise floor, short enough to still feel responsive.
    if(walked >= 4*STEP_M && win_d0 > 0 && dist_m > 0){
        float dd = dist_m - win_d0;              // negative = closing
        float r  = -dd / walked;                 // 1 = straight at it
        if(r >  1.0f) r =  1.0f;
        if(r < -1.0f) r = -1.0f;
        float th = acosf(r) * 180.0f/(float)M_PI;
        live_angle = (live_angle < 0) ? th : live_angle + 0.5f*(th-live_angle);
        live_valid = true;
        walked = 0; win_d0 = dist_m;             // slide the window
    }
}

/* ---- BLE, running concurrently with WiFi ----
   Same peer, second radio. WiFi gives us FTM (real distance) but costs power;
   BLE gives RSSI only but is far cheaper and is the only radio that can see an
   unmodified badge. Running both lets us compare them on identical geometry
   instead of arguing about it. */
static volatile int  ble_rssi = -127;
static volatile int  ble_pkts = 0;
static volatile int64_t ble_last_us = 0;
static float ble_f = -90.0f;
static bool  ble_up = false;
static volatile int ble_seen_any = 0;    // any advert at all -> scanner works
static uint8_t ble_own_addr_type = 0;
static int ble_adv_rc = -1, ble_disc_rc = -1, ble_init_rc = -99, ble_synced = 0;

static inline bool ble_fresh(void){
    return ble_last_us && (esp_timer_get_time() - ble_last_us) < 3000000;
}
// pull the Complete Local Name out of an advertising payload
static bool adv_name_is_peer(const uint8_t *d, uint8_t len){
    for(int i=0; i+1 < len; ){
        uint8_t fl = d[i], ft = d[i+1];
        if(fl == 0) break;
        if((ft == 0x09 || ft == 0x08) && fl >= 10){
            if(memcmp(&d[i+2], "HTN-FIND-", 9) == 0){
                // ignore our own advertisement
                if(memcmp(&d[i+2], my_ssid, strlen(my_ssid)) != 0) return true;
            }
        }
        i += fl + 1;
    }
    return false;
}
static int ble_gap_cb(struct ble_gap_event *ev, void *arg){
    if(ev->type == BLE_GAP_EVENT_DISC){
        ble_seen_any++;
        if(adv_name_is_peer(ev->disc.data, ev->disc.length_data)){
            ble_rssi = ev->disc.rssi;
            ble_pkts++;
            ble_last_us = esp_timer_get_time();
        }
    }
    return 0;
}
static void ble_start(void){
    struct ble_hs_adv_fields f; memset(&f,0,sizeof f);
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t*)my_ssid; f.name_len = strlen(my_ssid); f.name_is_complete = 1;
    ble_gap_adv_set_fields(&f);
    struct ble_gap_adv_params ap; memset(&ap,0,sizeof ap);
    ap.conn_mode = BLE_GAP_CONN_MODE_NON;       // broadcast only
    ap.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ap.itvl_min = 160; ap.itvl_max = 160;       // 100 ms
    ble_adv_rc = ble_gap_adv_start(ble_own_addr_type, NULL, BLE_HS_FOREVER,
                                   &ap, ble_gap_cb, NULL);

    struct ble_gap_disc_params dp; memset(&dp,0,sizeof dp);
    dp.passive = 1; dp.filter_duplicates = 0;
    dp.itvl = 160; dp.window = 160;             // scan continuously
    ble_disc_rc = ble_gap_disc(ble_own_addr_type, BLE_HS_FOREVER, &dp,
                               ble_gap_cb, NULL);
    ble_up = (ble_disc_rc == 0);

}
static void ble_on_sync(void){
    ble_synced = 1;
    // Must infer and KEEP the address type -- passing BLE_OWN_ADDR_PUBLIC when
    // the controller has no public address makes adv/disc fail silently.
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &ble_own_addr_type);
    ble_start();
}
static void ble_host_task(void *p){ nimble_port_run(); nimble_port_freertos_deinit(); }
static void ble_init(void){
    esp_err_t rc = nimble_port_init();
    ble_init_rc = (int)rc;          // reported by log_task; a printf here
                                    // lands in the lost first 1.6 s of boot
    if(rc != ESP_OK) return;
    ble_hs_cfg.sync_cb = ble_on_sync;
    // No ble_svc_gap_init() here: the GAP *service* is only built for the
    // peripheral role, which we deliberately disabled. Broadcast-only
    // advertising carries the name in the adv fields instead.
    nimble_port_freertos_init(ble_host_task);
}

/* ---------------- pages ---------------- */
/* ---- BEARING: one spin, three analyses on the SAME samples ----
   No UWB / one antenna / no magnetometer, so no true compass. But a body
   attenuates 2.4 GHz by 10-20 dB, so RSSI varies roughly sinusoidally as you
   turn. We bin one guided 360 spin into NB time bins and run three estimators
   over the identical data so they can be compared directly:

     MAX   - argmax bin. Absolute, so any drift during the spin corrupts it.
     D180  - argmax of R[i] - R[i+NB/2]. Antipodal differencing rejects
             common-mode (TX power, path loss, slow drift). Quantised to NB/2.
     DFT   - arg/abs of the first harmonic. Same idea as D180 but uses every
             sample optimally, giving a continuous angle plus a confidence.

   Weakness shared by all three: without a gyro we assume the spin is at a
   constant rate, so the angular axis is only as uniform as the user's turn.
   Hence the guided timer. */
#define NB 16
// Measured against known ground truth over 4 spins: all estimators showed a
// consistent +44 deg bias with only 16 deg scatter (DFT). The bias is dead
// time -- you press UP, then take ~1 s to start turning, and those stationary
// bins shift the whole pattern later. Fixed two ways: wait for motion before
// sampling (removes the variable part) and subtract the residual constant.
#define BEARING_BIAS 34.0f   // pooled over 8 spins vs known truth
#define SPIN_LEAD_MS 2000     // fixed countdown: a CONSISTENT delay beats a
                              // motion trigger, which starts at a random point
                              // in the turn and tripled the scatter.
#define SPIN_MS 8000
static bool  spin = false;           // capture in progress
static bool  spin_arm = false;       // counting down before capture
static int64_t arm_t0 = 0;
// Averaging several spins is the only reliable accuracy win: single-spin
// scatter is ~34 deg, and it falls as 1/sqrt(n). We accumulate the complex
// first-harmonic VECTORS, not the angles -- that is the correct way to mean
// circular data, and it weights each spin by its own amplitude for free.
static float acc_re = 0, acc_im = 0;
static int   acc_n = 0;
static float bearing_avg = -1;
static bool  spin_done = false;
static int64_t spin_t0 = 0;
static float bin_sum[NB]; static int bin_n[NB];
static float bearing_max = -1, bearing_d180 = -1, bearing_dft = -1, bearing_min180 = -1;
static float dft_conf = 0, spin_span = 0;

static void spin_start(void){
    for(int i=0;i<NB;i++){ bin_sum[i]=0; bin_n[i]=0; }
    spin_arm = true; spin = false; spin_done = false;
    arm_t0 = esp_timer_get_time();
}
static void spin_begin(void){
    spin_arm = false; spin = true; spin_t0 = esp_timer_get_time();
}
static void spin_finish(void){
    float R[NB]; float mx=-1e9, mn=1e9;
    for(int i=0;i<NB;i++){
        R[i] = bin_n[i] ? bin_sum[i]/bin_n[i] : -100.0f;
        if(R[i]>mx) mx=R[i];
        if(R[i]<mn) mn=R[i];
    }
    spin_span = mx - mn;

    int bi=0; for(int i=1;i<NB;i++) if(R[i]>R[bi]) bi=i;
    bearing_max = bi * 360.0f / NB;

    // The engineers' method, verbatim: weakest bin, then the opposite side.
    int ni=0; for(int i=1;i<NB;i++) if(R[i]<R[ni]) ni=i;
    bearing_min180 = fmodf(ni * 360.0f / NB + 180.0f, 360.0f);

    int di=0; float dbest=-1e9;
    for(int i=0;i<NB;i++){
        float d = R[i] - R[(i+NB/2)%NB];
        if(d>dbest){ dbest=d; di=i; }
    }
    bearing_d180 = di * 360.0f / NB;

    float re=0, im=0, mean=0;
    for(int i=0;i<NB;i++) mean += R[i];
    mean /= NB;
    // Fit using only the SHADOWED half of the sweep. Measured on 8 spins vs
    // known truth: the null is 1.7x sharper than the peak (5.44 dB vs 3.17 dB
    // curvature), and weighting toward it cut scatter monotonically,
    // 33.9 -> 31.3 -> 30.4 -> 28.9 deg. The body-shadow notch carries the
    // direction information; the unblocked arc is broad and says little.
    for(int i=0;i<NB;i++){
        if(R[i] > mean) continue;                // skip the unshadowed half
        float th = 2.0f*(float)M_PI*i/NB;
        re += (R[i]-mean)*cosf(th);
        im += (R[i]-mean)*sinf(th);
    }
    acc_re += re; acc_im += im; acc_n++;
    float aa = atan2f(acc_im,acc_re) * 180.0f/(float)M_PI - BEARING_BIAS;
    while(aa < 0) aa += 360.0f;
    while(aa >= 360.0f) aa -= 360.0f;
    bearing_avg = aa;

    float ang = atan2f(im,re) * 180.0f/(float)M_PI - BEARING_BIAS;
    while(ang < 0) ang += 360.0f;
    while(ang >= 360.0f) ang -= 360.0f;
    bearing_dft = ang;
    dft_conf = 2.0f*sqrtf(re*re+im*im)/NB;   // ~amplitude of the 1st harmonic
    // Publish the raw pattern so the bearing maths can be re-analysed offline
    // against known ground truth, instead of trusting three summary numbers.
    printf("SPIN bins=%d span=%.1f conf=%.1f dft=%.0f d180=%.0f max=%.0f min180=%.0f raw=",
           NB,(double)spin_span,(double)dft_conf,(double)bearing_dft,
           (double)bearing_d180,(double)bearing_max,(double)bearing_min180);
    for(int i=0;i<NB;i++) printf("%.1f%s", (double)R[i], i<NB-1?",":"\n");
    live_side = (bearing_avg > 180.0f) ? -1 : +1;   // spin picks left vs right
    live_angle = -1; live_valid = false;
    walked = 0; win_d0 = -1;
    spin = false; spin_done = true;
}

static int page = 0;   // 0 = target list, 1 = finder
static uint8_t btn = 0;
static int moving = 0;             // accelerometer motion score
static const char *trend_txt = "hold still";
static uint16_t trend_col;

static void hdr(const char *t){
    rect(0,0,W,26,C_HDR);
    text(6,6,t,C_ACC,2);
    rect(0,26,W,2,C_ACC);
    rect(0,H-20,W,20,C_HDR);
}
static void draw_list(void){
    hdr("PICK A TARGET");
    { char me[32]; snprintf(me,sizeof me,"me: %s",my_ssid);
      text(180,8,me,C_DIM,1); }
    text(6,H-15,"UP/DN pick  A lock  B rescan",C_DIM,1);
    if(!nap){ text(10,110,"scanning...",C_FG,2); return; }
    int y=34;
    for(int i=0;i<nap && y<H-26;i++,y+=17){
        bool cur = (i==sel);
        if(cur) rect(2,y-2,316,17,C_BAR);
        char s[26]; snprintf(s,sizeof s,"%.16s",(char*)aps[i].ssid);
        if(!s[0]) snprintf(s,sizeof s,"(hidden)");
        bool peer = (strncmp((char*)aps[i].ssid,"HTN-FIND-",9)==0);
        text(6,y,s,peer?C_OK:(cur?C_FG:C_DIM),1);
        char r[10]; snprintf(r,sizeof r,"%d",aps[i].rssi);
        text(210,y,r,cur?C_FG:C_DIM,1);
        if(aps[i].ftm_responder) text(250,y,"FTM",C_OK,1);
        char c[8]; snprintf(c,sizeof c,"c%d",aps[i].primary);
        text(290,y,c,C_DIM,1);
    }
}
static void draw_find(void){
    hdr("FINDER");
    if(!spin && !spin_done)
        text(6,H-15,"A:FTM  UP:360 bearing scan  B:back",C_DIM,1);
    if(!have_target){ text(10,110,"no target",C_DIM,2); return; }

    if(spin_arm){
        int left = (int)((SPIN_LEAD_MS*1000 - (esp_timer_get_time()-arm_t0))/1000000) + 1;
        char c[12];
        if(left < 1) left = 1;
        if(left > 9) left = 9;
        snprintf(c,sizeof c,"%d",left);
        text(140,80,c,C_ACC,2);
        text(6,120,"get ready - start turning on GO",C_DIM,1);
        text(6,140,"clockwise, badge flat on your chest",C_DIM,1);
        return;
    }
    if(spin){
        int64_t el = esp_timer_get_time() - spin_t0;
        float pr = (float)el / (SPIN_MS*1000.0f);
        if(pr>1) pr=1;
        text(6,60,"TURN 360 SLOWLY",C_ACC,2);
        rect(4,92,312,24,C_HDR);
        rect(6,94,(int)(308*pr),20,C_OK);
        char q[40]; snprintf(q,sizeof q,"%.1fs left  keep badge at your chest",
                             (double)((SPIN_MS/1000.0f)*(1.0f-pr)));
        text(6,122,q,C_DIM,1);
        for(int i=0;i<NB;i++){                       // live polar-ish bars
            int v = bin_n[i] ? (int)(bin_sum[i]/bin_n[i]) + 100 : 0;
            if(v<0) v=0;
            if(v>70) v=70;
            rect(6+i*19, 200-v/2, 16, v/2+2, C_BAR);
        }
        return;
    }
    if(spin_done){
        char q[48];
        text(6,40,"BEARINGS (deg from spin start)",C_DIM,1);
        uint16_t col = acc_n>=3 ? C_OK : C_DIM;
        // Dial: 12 o'clock is where you began the spin. The needle is the way
        // to walk. Turning clockwise by this many degrees points you at it.
        int cx=252, cy=120, rr=54;
        for(int a=0;a<360;a+=30){
            float th=a*(float)M_PI/180.0f;
            rect(cx+(int)((rr+6)*sinf(th))-1, cy-(int)((rr+6)*cosf(th))-1,3,3,C_HDR);
        }
        rect(cx-2,cy-rr-14,4,8,C_DIM);                       // 12 o'clock mark
        float th = bearing_avg*(float)M_PI/180.0f;
        thick_line(cx,cy, cx+(int)(rr*sinf(th)), cy-(int)(rr*cosf(th)), 5, col);
        rect(cx-4,cy-4,8,8,C_FG);

        int turn = (int)bearing_avg;
        snprintf(q,sizeof q,"TURN %d", turn<=180?turn:360-turn);
        text(6,44,q,col,2);
        text(6,70, turn<=180 ? "degrees RIGHT" : "degrees LEFT", col,2);
        text(6,98,"then walk forward",C_DIM,1);
        snprintf(q,sizeof q,"avg %d spin%s  +-%d deg", acc_n, acc_n==1?"":"s",
                 (int)(29.0f/sqrtf((float)acc_n)));
        text(6,116,q,col,1);
        if(acc_n<3) text(6,132,"spin again to tighten",C_DIM,1);
        snprintf(q,sizeof q,"null+180 %3.0f   dft %3.0f",
                 (double)bearing_min180,(double)bearing_dft);
        text(6,150,q,C_DIM,1);
        snprintf(q,sizeof q,"d180 %3.0f  max %3.0f  conf %.1f",
                 (double)bearing_d180,(double)bearing_max,(double)dft_conf);
        text(6,166,q,C_DIM,1);
        snprintf(q,sizeof q,"span %.0f dB  %s",(double)spin_span,
                 spin_span<5.0f ? "TOO WEAK - retry" : "usable");
        text(6,182,q, spin_span<5.0f ? C_ACC : C_OK, 1);
        for(int i=0;i<NB;i++){
            int v = bin_n[i] ? (int)(bin_sum[i]/bin_n[i]) + 100 : 0;
            if(v<0) v=0;
            if(v>70) v=70;
            rect(6+i*19, 214-v/2, 16, v/2+2,
                 (i==(int)(bearing_dft*NB/360))?C_OK:C_BAR);
        }
        text(6,H-15,"UP: add another spin  DOWN: reset  B: back",C_DIM,1);
        return;
    }


    char s[40];
    snprintf(s,sizeof s,"%.20s",(char*)target.ssid);
    text(6,32,s[0]?s:"(hidden)",C_FG,1);

    // big trend word - this is the "direction" substitute
    text(6,52,trend_txt,trend_col,2);

    // distance estimate from path loss
    float m = rssi_to_m(rssi_f);
    if(m < 100.0f) snprintf(s,sizeof s,"~%.1f m", (double)m);
    else           snprintf(s,sizeof s,"~far");
    text(6,80,s,C_FG,2);
    snprintf(s,sizeof s,"WIFI %.0f dBm  %s %d/s",(double)rssi_f,
             now_fresh()?"NOW":"bcn", now_fresh()?now_pkts:rssi_pkts);
    text(6,106,s,C_OK,1);
    if(!ble_up)          snprintf(s,sizeof s,"BLE  off");
    else if(!ble_fresh())snprintf(s,sizeof s,"BLE  -- no peer seen");
    else                 snprintf(s,sizeof s,"BLE  %.0f dBm  %d/s",(double)ble_f,ble_pkts);
    text(6,122,s, ble_fresh()?C_ACC:C_DIM,1);
    { int wq=(int)rssi_f+100, bq=(int)ble_f+100;
      if(wq<0) wq=0;
      if(wq>70) wq=70;
      if(bq<0) bq=0;
      if(bq>70) bq=70;
      rect(200,106,wq*3/2,8,C_OK);
      rect(200,122,bq*3/2,8,C_ACC); }

    // FTM: the real measurement, when the AP supports it
    if(target_has_ftm){
        if(ftm_state==2){ snprintf(s,sizeof s,"FTM %.2f m",(double)(ftm_cm/100.0f));
                          text(6,126,s,C_OK,2); }
        else if(ftm_state==1) text(6,126,"FTM ranging...",C_DIM,2);
        else if(ftm_state==3) text(6,126,"FTM failed",C_ACC,1);
        else                  text(6,126,"A = FTM ping",C_DIM,1);
    } else {
        text(6,126,"AP has no FTM (rssi only)",C_DIM,1);
    }

    // live compass - updates as you walk, no spin needed
    if(live_valid){
        int cx=252, cy=150, rr=44;
        for(int a=0;a<360;a+=45){
            float th2=a*(float)M_PI/180.0f;
            rect(cx+(int)((rr+6)*sinf(th2))-1, cy-(int)((rr+6)*cosf(th2))-1,3,3,C_HDR);
        }
        float th = live_side*live_angle*(float)M_PI/180.0f;
        uint16_t lc = live_angle<30 ? C_OK : (live_angle<90 ? C_BAR : C_ACC);
        thick_line(cx,cy, cx+(int)(rr*sinf(th)), cy-(int)(rr*cosf(th)), 5, lc);
        rect(cx-3,cy-3,6,6,C_FG);
        char m[40];
        if(live_angle < 25)      snprintf(m,sizeof m,"ON TRACK");
        else if(live_angle > 140) snprintf(m,sizeof m,"TURN AROUND");
        else snprintf(m,sizeof m,"%d deg %s",(int)live_angle, live_side>0?"RIGHT":"LEFT");
        text(6,150,m,lc,2);
        snprintf(m,sizeof m,"%d steps  %.1f m away",steps,(double)dist_m);
        text(6,176,m,C_DIM,1);
    } else {
        text(6,150,"walk a few steps",C_DIM,2);
        text(6,176,"live compass needs motion to lock",C_DIM,1);
    }

    // signal history sparkline
    int gy = 158, gh = 54;
    rect(4,gy,312,gh,C_HDR);
    for(int i=0;i<HIST;i++){
        int v = hist[(hist_w+i)%HIST];          // -100..-30 -> 0..gh
        int hgt = (v+100)*gh/70; if(hgt<1)hgt=1; if(hgt>gh)hgt=gh;
        rect(6+i*3, gy+gh-hgt, 2, hgt, hgt>gh/2?C_OK:C_BAR);
    }
}
static void (*PAGEFN[2])(void) = { draw_list, draw_find };

/* LEDs: proximity ring. Brighter and greener the closer you are. */
static void leds_tick(int t){
    led_strip_clear(strip);
    if(page==1 && have_target && spin){
        int64_t el = esp_timer_get_time() - spin_t0;
        int lit = (int)(6.0f*el/(SPIN_MS*1000.0f));
        if(lit>6) lit=6;
        for(int i=0;i<lit;i++) led_strip_set_pixel(strip,i,0,14,4);
        led_strip_refresh(strip);
        return;
    }
    if(page==1 && have_target){
        int q = (int)(rssi_f) + 100;            // ~0..70
        if(q<0) q=0;
        if(q>70) q=70;
        int lit = 1 + q*6/70;
        if(lit>6) lit=6;
        int v = 4 + q/3;
        for(int i=0;i<lit;i++){
            if(q>55)      led_strip_set_pixel(strip,i,0,v,v/4);   // close: green
            else if(q>35) led_strip_set_pixel(strip,i,v/2,v/2,0); // mid: amber
            else          led_strip_set_pixel(strip,i,v/2,0,v/3); // far: dim magenta
        }
        if(q>60 && (t/3)%2) led_strip_set_pixel(strip,(t/2)%6,0,30,0);
    } else {
        led_strip_set_pixel(strip,(t/4)%6,3,3,8);
    }
    led_strip_refresh(strip);
}

// USB-Serial-JTAG printf BLOCKS when the buffer fills and no host is draining
// it -- which is exactly the case on battery. Any printf on the main loop then
// freezes the display. So ALL logging happens on this low-priority task: if it
// stalls, the UI, radio and buttons carry on regardless.
static void log_task(void *arg){
    for(;;){
        if(have_target)
            printf("find %-14s src=%s rssi=%.0f %-10s ~%.1fm ftm=%lucm span=%.0f\n",
                   (char*)target.ssid, now_fresh()?"espnow":"beacon",
                   (double)rssi_f, trend_txt, (double)rssi_to_m(rssi_f),
                   (unsigned long)ftm_cm, (double)spin_span);
        else if(0) {}
        else
            printf("idle: %d APs | BLE init=%d sync=%d adv=%d disc=%d seen=%d peer=%s %d\n",
                   nap, ble_init_rc, ble_synced, ble_adv_rc, ble_disc_rc,
                   ble_seen_any, ble_fresh()?"yes":"no", (int)ble_f);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void){
    // The WiFi driver logs 3 WARN lines per FTM session. At several sessions a
    // second that floods the console, and USB-Serial-JTAG printf BLOCKS when no
    // host is draining it -- so on battery the main loop stalls and the screen
    // freezes. Silence it.
    esp_log_level_set("wifi", ESP_LOG_ERROR);
    esp_log_level_set("wifi_init", ESP_LOG_ERROR);
    esp_log_level_set("pp", ESP_LOG_ERROR);
    nvs_flash_init();
    fb = heap_caps_malloc(W*STRIPE*sizeof(uint16_t), MALLOC_CAP_DMA);
    buttons_init(); leds_init();
    accel_ok = accel_init();
    display_init();
    page_draw = draw_list;
    wifi_up();
    ble_init();
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_FTM_REPORT, ftm_evt, NULL);
    do_scan();

    xTaskCreate(log_task, "log", 3072, NULL, 1, NULL);   // prio 1: below the UI

    uint8_t prev=0; int t=0; int last_ax=0,last_ay=0;
    int64_t last_grad = esp_timer_get_time();
    while(1){
        uint8_t b = buttons(); uint8_t edge = b & ~prev; prev = b; btn = b;
        if(page==0){
            if(edge&(1u<<BTN_DOWN)) sel = nap ? (sel+1)%nap : 0;
            if(edge&(1u<<BTN_UP))   sel = nap ? (sel+nap-1)%nap : 0;
            if(edge&(1u<<BTN_B))    do_scan();
            if((edge&(1u<<BTN_A)) && nap){ lock_target(); page=1; }
        } else {
            if(edge&(1u<<BTN_B)){ esp_wifi_set_promiscuous(false); page=0; do_scan(); }
            if(edge&(1u<<BTN_A))  ftm_go();
            if(edge&(1u<<BTN_UP)) spin_start();
            if(edge&(1u<<BTN_DOWN)){ spin=false; spin_arm=false; spin_done=false;
                                     acc_re=acc_im=0; acc_n=0; bearing_avg=-1; }
        }

        // Prefer the 50 Hz ESP-NOW stream; fall back to 10 Hz beacons for
        // plain APs that cannot talk back.
        int src = now_fresh() ? now_rssi : rssi_raw;

        accel_read();
        {   static float amag_f = 1000; static bool up = false; static int refr = 0;
            float amag = sqrtf((float)(ax*ax + ay*ay + az*az));
            amag_f += 0.3f*(amag - amag_f);
            if(refr > 0) refr--;
            if(!up && amag_f > 1150 && refr == 0){ up = true; }
            else if(up && amag_f < 1050){ up = false; refr = 8; step_detected(); }
        }
        int dmot = abs(ax-last_ax) + abs(ay-last_ay);
        last_ax=ax; last_ay=ay;
        moving = (dmot > 60) ? 12 : (moving>0 ? moving-1 : 0);

        if(have_target){
            float dm = (target_has_ftm && ftm_cm > 0) ? ftm_cm/100.0f
                                                      : rssi_to_m(rssi_f);
            dist_m = (dist_m < 0) ? dm : dist_m + 0.25f*(dm - dist_m);
            if(ble_fresh()) ble_f += 0.25f*(ble_rssi - ble_f);
            // Adaptive: track hard while moving, smooth hard while still.
            float k = moving ? 0.45f : 0.15f;
            rssi_f    += k     * (src - rssi_f);
            rssi_slow += 0.05f * (src - rssi_slow);
            if(spin_arm && esp_timer_get_time()-arm_t0 > (int64_t)SPIN_LEAD_MS*1000)
                spin_begin();
            if(spin){
                int64_t el = esp_timer_get_time() - spin_t0;
                if(el >= (int64_t)SPIN_MS*1000) spin_finish();
                else { int b = (int)(el * NB / ((int64_t)SPIN_MS*1000));
                       if(b>=0 && b<NB){ bin_sum[b]+=rssi_f; bin_n[b]++; } }
            }
            if(ftm_auto && target_has_ftm && (t % 50) == 0 && !spin) ftm_go();
            if(esp_timer_get_time() - last_grad > 150000){
                last_grad = esp_timer_get_time();
                hist[hist_w] = (int8_t)rssi_f; hist_w = (hist_w+1)%HIST;
                float d = rssi_f - rssi_slow;
                (void)0;
                // Gate on motion: RSSI noise looks like a gradient when still.
                if(!moving)      { trend_txt = "hold still"; trend_col = C_DIM; }
                else if(d >  1.5f){ trend_txt = "WARMER";    trend_col = C_OK;  }
                else if(d < -1.5f){ trend_txt = "COLDER";    trend_col = C_ACC; }
                else              { trend_txt = "...";       trend_col = C_DIM; }
                rssi_pkts = 0; now_pkts = 0;
            }
        }
        // auto-rescan on the target list: keeps peers fresh as they move,
        // and makes the badge observable over serial without a button press
        if(page==0 && (t % 400) == 399) do_scan();

        // our own ranging beacon for the peer to measure
        if(page==1){ uint8_t ping[4] = {'F','N','D',(uint8_t)t};
                     esp_now_send(BCAST, ping, sizeof ping); }

        // A full 320x240 blit costs ~31 ms of SPI. Redrawing every tick would
        // cap the tracking loop at ~16 Hz, so only redraw every 3rd tick and
        // let sampling/filtering run at ~50 Hz.
        page_draw = PAGEFN[page];
        if((t % 3) == 0) flush();
        if((t % 3) == 1) leds_tick(t);
        t++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
