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
static volatile int64_t ftm_started_us = 0;
static volatile int64_t ftm_ok_us = 0;
static volatile int ftm_state = 0;   // 0 idle, 1 in progress, 2 ok, 3 fail
static bool target_has_ftm = false;

static float rssi_to_m(float r){
    return powf(10.0f, (RSSI_AT_1M - r) / (10.0f * PATHLOSS_N));
}

// Promiscuous sniffing gives ~10 beacons/sec from the target, vs ~1 scan every
// 2 s with active scanning. Much better gradient resolution.
static void vs_beacon(const uint8_t *f, int rssi);
static volatile bool vs_on;
static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type){
    if(type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *p = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t *f = p->payload;
    if((f[0] & 0xFC) != 0x80) return;              // beacon only
    if(vs_on){ vs_beacon(f, p->rx_ctrl.rssi); return; }
    if(memcmp(f + 10, target.bssid, 6) != 0) return; // addr2 == target
    rssi_raw = p->rx_ctrl.rssi;
    rssi_pkts++;
}

// Every badge is BOTH a beacon and a seeker: it runs a SoftAP with the
// 802.11mc FTM responder enabled, so the other badge can range to it, while
// also scanning and ranging itself. Symmetric - either can find the other.
#define FIND_CHAN 1
static char my_ssid[24];
static char my_name[12];

static volatile int ftm_seq = 0;   // bumped by the FTM report handler
static int pin_rc = -99;
static void pin_channel(void){        // both badges must share a channel for
    pin_rc = esp_wifi_set_channel(NOW_CHAN, WIFI_SECOND_CHAN_NONE); // ESP-NOW
}
static int   tri_stage = 0;      // 0 idle, 1 captured A1, 2 solved
static float tri_walk = 0;       // baseline walked between captures, m
static int   tri_steps = 0;
static int   tri_burst_n = 0;
static float tri_burst_spread = 0;

/* forward decls: the ESP-NOW rx callback below handles triangulation control
   messages, but the triangulation state lives further down the file. */
#define MAXPEERAP 16
typedef struct { uint8_t bssid[6]; int8_t rssi; } ap_ent_t;
static ap_ent_t peer_aps[MAXPEERAP];
static int peer_nap = 0;
static volatile bool peer_scan_fresh = false;
static volatile bool scan_req_pending = false;
static volatile bool inbound_req;
static volatile bool auto_regrant;
static uint8_t granted_to[6];      // badge we have already approved
static bool    have_granted;
static char  inbound_name[12];
static uint8_t inbound_mac[6];
enum { CONSENT_NONE=0, CONSENT_PENDING, CONSENT_GRANTED, CONSENT_DENIED };
static int   consent;
static volatile int n_req_rx=0, n_resp_rx=0;      // what this badge received
static int n_req_tx=0, n_resp_tx=0;               // what it sent
static int rc_req_tx=-99, rc_resp_tx=-99, peer_add_rc=-99;
static uint8_t peer_mac_g[6];         // esp_now_send return codes

/* ---- ESP-NOW ranging: no 102 ms beacon floor, so we can sample at 50 Hz ---- */
static uint8_t BCAST[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
static volatile int  now_rssi = -100;
static volatile int  now_pkts = 0;
static volatile int64_t now_last_us = 0;
static uint8_t peer_mac[6];
static bool peer_known = false;

static void now_rx(const esp_now_recv_info_t *i, const uint8_t *d, int len){
    if(len >= 2 && d[0] == 0xA1){
        n_req_rx++;
        if(!peer_known){ memcpy(peer_mac, i->src_addr, 6); }
        memcpy(peer_mac_g, i->src_addr, 6);
        scan_req_pending = true; return;
    }
    if(len >= 2 && d[0] == 0xA3){                 // someone asks to find us
        int n = d[1]; if(n > 11) n = 11;
        memcpy(inbound_name, d+2, n); inbound_name[n] = 0;
        memcpy(inbound_mac, i->src_addr, 6);
        // already said yes to this badge? re-confirm silently, do not nag
        if(have_granted && !memcmp(granted_to, i->src_addr, 6)) auto_regrant = true;
        else inbound_req = true;
        return;
    }
    if(len >= 2 && d[0] == 0xA4){ consent = CONSENT_GRANTED; return; }
    if(len >= 2 && d[0] == 0xA5){ consent = CONSENT_DENIED;  return; }
    if(len >= 2 && d[0] == 0xA2){
        n_resp_rx++;
        int n = d[1]; if(n > 16) n = 16;
        if(len >= 2 + n*7){
            memcpy(peer_aps, d+2, n*7);
            peer_nap = n; peer_scan_fresh = true;
        }
        return;
    }
    if(peer_known && memcmp(i->src_addr, peer_mac, 6) != 0) return;
    if(i->rx_ctrl) now_rssi = i->rx_ctrl->rssi;
    now_pkts++;
    now_last_us = esp_timer_get_time();
}
static void espnow_up(void){
    if(esp_now_init() != ESP_OK) return;
    esp_now_register_recv_cb(now_rx);
    esp_now_peer_info_t p = { .channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false };
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
    // One binary, two badges: pick the owner's name from the MAC prefix.
    if(m[0]==0xe8 && m[1]==0xf6 && m[2]==0x0a)      strcpy(my_name,"Ri");
    else if(m[0]==0x28 && m[1]==0x84 && m[2]==0x85) strcpy(my_name,"Lin");
    else snprintf(my_name, sizeof my_name, "%02X%02X", m[4], m[5]);
    snprintf(my_ssid, sizeof my_ssid, "HTN-FIND-%s", my_name);

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
static bool scan_silent = false;   // leave the last frame on screen instead
static void do_scan(void){
    // a scan blocks the UI for ~2 s. On the triangulate pages a wipe is more
    // jarring than a still frame, so those callers freeze what is there.
    if(fb && !scan_silent){
        for(fb_y0=0; fb_y0<H; fb_y0+=STRIPE){
            for(int i=0;i<W*STRIPE;i++) fb[i]=C_BG;
            text(90,108,"scanning...",C_FG,2);
            esp_lcd_panel_draw_bitmap(panel,0,fb_y0,W,fb_y0+STRIPE,fb);
            xSemaphoreTake(blit_done, pdMS_TO_TICKS(200));
        }
    }
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
    // A scan hops every channel and leaves us on the last one. ESP-NOW only
    // works when both badges sit on the SAME channel, so always come home --
    // otherwise a badge idling on the target list (which auto-rescans) drifts
    // away and silently stops hearing its peer.
    pin_channel();
    {   wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
        esp_wifi_set_promiscuous_filter(&f);
        esp_wifi_set_promiscuous_rx_cb(promisc_cb);
        esp_wifi_set_promiscuous(true); }
}
static void send_find_req(void);
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
    if(peer_known){
        // Unicast is acknowledged at the MAC layer, so esp_now_send's status
        // callback becomes meaningful. Broadcast reports success regardless of
        // whether anything heard it, which is why this was so hard to debug.
        esp_now_peer_info_t up = { .channel = NOW_CHAN, .ifidx = WIFI_IF_STA,
                                   .encrypt = false };
        memcpy(up.peer_addr, peer_mac, 6);
        if(esp_now_is_peer_exist(peer_mac)) esp_now_del_peer(peer_mac);
        peer_add_rc = esp_now_add_peer(&up);
        send_find_req();
    }
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
    ftm_seq++;
    if(r->status == FTM_STATUS_SUCCESS){
        // Multipath can only ever make the estimate LONGER, never shorter, and
        // a human cannot close 1.5 m/s. Reject physically impossible jumps.
        uint32_t prev = ftm_cm;
        if(prev == 0 || (uint32_t)abs((int)r->dist_est - (int)prev) < 400)
            ftm_cm = r->dist_est;
        else if(r->dist_est < prev) ftm_cm = r->dist_est;   // shorter = likelier LOS
        ftm_state = 2;
        ftm_ok_us = esp_timer_get_time();
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
    ftm_started_us = esp_timer_get_time();
}

/* The closing-rate live compass was removed. The bearing is now a one-shot
   calibration: spin once, it tells you which way to start, and it stays put.
   Step counting is kept only to show how far you have walked. */
static float dist_m = -1;            // fused range estimate, metres
static int   steps = 0;
#define STEP_M 0.72f

static void step_detected(void){
    steps++;
    // Stage 1 -> 2 needs a baseline, and the only sensor that can measure one
    // is the accelerometer. Without this the counter sat at zero and the solve
    // always fell back to its assumed 3 m.
    if(tri_stage == 1){ tri_steps++; tri_walk = tri_steps * STEP_M; }
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



/* Only the DIFFERENCE between the two captures matters, so FTM's constant
   bias cancels and we are left with random noise -- which averages down.
   A single reading (the old behaviour) was the worst possible estimator:
   1/20th of the available information, and sometimes a stale global. */
#define FTM_BURST 20
static int ftm_burst_want = FTM_BURST;
static float ftm_burst_median(void){
    float v[FTM_BURST]; int n = 0;
    int want = ftm_burst_want < 1 ? 1 :
               (ftm_burst_want > FTM_BURST ? FTM_BURST : ftm_burst_want);
    int64_t deadline = esp_timer_get_time() + 12000000;   // 12 s cap
    while(n < want && esp_timer_get_time() < deadline){
        int seq0 = ftm_seq;
        ftm_state = 0;
        ftm_go();
        int64_t t0 = esp_timer_get_time();
        while(ftm_seq == seq0 && esp_timer_get_time() - t0 < 500000)
            vTaskDelay(pdMS_TO_TICKS(10));
        if(ftm_seq != seq0 && ftm_cm > 0){
            v[n++] = ftm_cm / 100.0f;
            tri_burst_n = n;
        }
    }
    if(n == 0) return -1;
    for(int i=1;i<n;i++){                  // insertion sort, n<=20
        float k=v[i]; int j=i-1;
        while(j>=0 && v[j]>k){ v[j+1]=v[j]; j--; }
        v[j+1]=k;
    }
    tri_burst_spread = v[n-1] - v[0];
    printf("BURST n=%d min=%.2f max=%.2f med=%.2f raw=", n,(double)v[0],
           (double)v[n-1], (double)((n&1)?v[n/2]:0.5f*(v[n/2-1]+v[n/2])));
    for(int i=0;i<n;i++) printf("%.2f%s", (double)v[i], i<n-1?",":"\n");
    return (n & 1) ? v[n/2] : 0.5f*(v[n/2-1] + v[n/2]);
}


/* ---- consent: you must agree before someone can track you ----
   Ri selects Lin -> Ri sends FIND_REQ carrying Ri's name -> Lin sees a prompt
   and accepts or declines -> Lin replies GRANT or DENY. Until Ri holds a
   grant, the finder shows a waiting state and tracks nothing. */
#define MSG_FIND_REQ   0xA3
#define MSG_FIND_GRANT 0xA4
#define MSG_FIND_DENY  0xA5

static int  consent = CONSENT_NONE;          // our state as the seeker
static void consent_init(void){ consent = CONSENT_NONE; }
static int64_t consent_sent_us = 0;

static void ensure_peer(const uint8_t *mac);

static void send_find_req(void){
    uint8_t m[14]; m[0] = MSG_FIND_REQ;
    m[1] = (uint8_t)strlen(my_name);
    memcpy(m+2, my_name, m[1]);
    esp_now_send(peer_known ? peer_mac : BCAST, m, 2 + m[1]);
    consent = CONSENT_PENDING;
    consent_sent_us = esp_timer_get_time();
}
/* esp_now_send() to an unregistered MAC fails with ESP_ERR_ESPNOW_NOT_FOUND
   and transmits nothing. The requester is not a peer of ours yet, so add them
   before replying -- this is why "allow" appeared to do nothing. */
static int consent_tx_rc = -99;
static void ensure_peer(const uint8_t *mac){
    if(esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t p = { .channel = NOW_CHAN, .ifidx = WIFI_IF_STA,
                              .encrypt = false };
    memcpy(p.peer_addr, mac, 6);
    esp_now_add_peer(&p);
}
static void send_consent_reply(bool ok){
    uint8_t m[2] = { ok ? MSG_FIND_GRANT : MSG_FIND_DENY, 0 };
    ensure_peer(inbound_mac);
    consent_tx_rc = esp_now_send(inbound_mac, m, 2);
    // belt and braces: broadcast it too, so a peer-table problem cannot
    // silently strand the requester on the asking screen
    esp_now_send(BCAST, m, 2);
    if(ok){ memcpy(granted_to, inbound_mac, 6); have_granted = true; }
    inbound_req = false;
}


/* ---- synthetic AP geometry ----
   The AP identities, count and RSSI are real (scanned, and exchanged with the
   peer). Their POSITIONS are synthesised, because RSSI-derived distance is
   unusable indoors: unknown transmit power alone is ~10 dB, i.e. ~135%
   distance error, and a few metres of baseline against APs tens of metres
   away amplifies what is left.

   What stays real: the range to B (FTM, ~1 m) and therefore the bearing.
   What is synthetic: where the APs are drawn. Derived deterministically from
   each BSSID so a given AP always lands in the same place. The screen says
   "AP geom: sim" so this is never mistaken for a measurement. */
static bool ap_geom_sim = true;
#define NFAKE 4
#define LIN_DEG 50.0f
static const float fake_k  [NFAKE] = { 2.1f,  1.25f, 2.6f,  1.65f };  // x Lin's range
static const float fake_deg[NFAKE] = { 32.0f, 70.0f, 112.0f, 155.0f };
#define FAKE_KMAX 2.6f

static float sim_ap_range(const uint8_t *bssid, int which){
    uint32_t h = 2166136261u;                  // FNV-1a over the BSSID
    for(int i=0;i<6;i++){ h ^= bssid[i]; h *= 16777619u; }
    if(which) h ^= 0x9e3779b9u;
    return 6.0f + (float)(h % 1400) / 100.0f;  // 6 .. 20 m, stable per AP
}

/* ================= AP TRIANGULATION =================
   Two badges, plus the building's access points as shared reference points.

   Frame: A1 at the origin, and the direction the user WALKS defines +x. That
   is the whole trick -- with no compass we cannot know north, but the user can
   always feel "straight ahead", so we solve in their walking frame and report
   the answer as an angle off it.

   Measurements per capture:
     dAB   FTM range A->B
     a_i   RSSI range A->AP i
     b_i   RSSI range B->AP i   (B scans on request and replies over ESP-NOW)

   A1, A2 and the two FTM ranges already pin B to two mirrored points. The APs
   do not add a new capability -- they over-determine the system, so a single
   bad range stops dominating. See the honest note on reflection below. */

typedef struct { uint8_t type; uint8_t n; ap_ent_t e[MAXPEERAP]; } scanmsg_t;
#define MSG_SCAN_REQ  0xA1
#define MSG_SCAN_RESP 0xA2

// our own scans at the two capture points
static ap_ent_t capA[2][MAXPEERAP]; static int capA_n[2] = {0,0};
static float capA_dab[2] = {-1,-1};
static float tri_angle = -1, tri_quality = -1;
static int tri_common = 0;
static const char *tri_msg = "READY";

static float rssi_range(int rssi){        // shared path-loss model
    return powf(10.0f, (RSSI_AT_1M - (float)rssi) / (10.0f * PATHLOSS_N));
}
// range to an AP: synthetic when ap_geom_sim, otherwise the (poor) path-loss
static float ap_range(const uint8_t *bssid, int rssi, int which){
    return ap_geom_sim ? sim_ap_range(bssid, which) : rssi_range(rssi);
}
static void snapshot_aps(ap_ent_t *dst, int *n){
    int k = 0;
    for(int i=0;i<nap && k<MAXPEERAP;i++){
        memcpy(dst[k].bssid, aps[i].bssid, 6);
        dst[k].rssi = (int8_t)aps[i].rssi;
        k++;
    }
    *n = k;
}
static void send_scan_req(void){
    scanmsg_t m; m.type = MSG_SCAN_REQ; m.n = 0;
    const uint8_t *dst = peer_known ? peer_mac : BCAST;
    rc_req_tx = esp_now_send(dst, (uint8_t*)&m, 2);
    n_req_tx++;
}
static void send_scan_resp(void){
    scanmsg_t m; m.type = MSG_SCAN_RESP;
    int n; snapshot_aps(m.e, &n); m.n = (uint8_t)n;
    // reply to whoever asked; fall back to broadcast if we have no lock
    const uint8_t *dst = peer_known ? peer_mac : BCAST;
    rc_resp_tx = esp_now_send(dst, (uint8_t*)&m, 2 + n*sizeof(ap_ent_t));
    n_resp_tx++;
}

/* Solve for B's bearing in the walking frame.
   A1=(0,0), A2=(s,0). B lies on the intersection of circles r=dAB1 about A1
   and r=dAB2 about A2. Returns |angle| off the walk direction; the sign is NOT
   recoverable from ranges alone (see note). */
static bool tri_solve(float s, float *out_ang, float *out_q){
    float d1 = capA_dab[0], d2 = capA_dab[1];
    if(d1 <= 0 || d2 <= 0 || s <= 0.1f) return false;
    float x = (d1*d1 - d2*d2 + s*s) / (2.0f*s);
    float h2 = d1*d1 - x*x;
    if(h2 < 0){                       // ranges inconsistent - clamp to closest
        h2 = 0;
        if(x >  d1) x =  d1;
        if(x < -d1) x = -d1;
    }
    float y = sqrtf(h2);
    float ang = atan2f(y, x) * 180.0f/(float)M_PI;   // 0 = dead ahead

    // Quality: how well do the common APs agree with this solution? For each
    // AP we place it from our own two ranges, then check B's reported range.
    float err = 0; int used = 0;
    for(int i=0;i<capA_n[0];i++){
        int j1=-1, j2=-1;
        for(int k=0;k<capA_n[1];k++)  if(!memcmp(capA[0][i].bssid,capA[1][k].bssid,6)) j1=k;
        for(int k=0;k<peer_nap;k++)   if(!memcmp(capA[0][i].bssid,peer_aps[k].bssid,6)) j2=k;
        if(j1<0 || j2<0) continue;
        float r1 = ap_range(capA[0][i].bssid, capA[0][i].rssi, 0);
        float r2 = ap_range(capA[0][i].bssid, capA[1][j1].rssi, 1);
        float rb = ap_range(capA[0][i].bssid, peer_aps[j2].rssi, 2);
        float px = (r1*r1 - r2*r2 + s*s) / (2.0f*s);
        float ph2 = r1*r1 - px*px;
        if(ph2 < 0) continue;
        float py = sqrtf(ph2);                   // same side as B by convention
        float bx = d1*cosf(ang*(float)M_PI/180.0f);
        float by = d1*sinf(ang*(float)M_PI/180.0f);
        float pred = sqrtf((px-bx)*(px-bx) + (py-by)*(py-by));
        err += fabsf(pred - rb);
        used++;
    }
    tri_common = used;
    *out_ang = ang;
    *out_q = used ? (err/used) : -1;             // mean |residual| in metres
    return true;
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
    { char me[32]; snprintf(me,sizeof me,"you are %s",my_name);
      text(170,10,me,C_DIM,1); }
    text(6,H-15,"UP/DN pick  A lock  B rescan",C_DIM,1);
    if(!nap){ text(10,110,"scanning...",C_FG,2); return; }
    int y=34;
    for(int i=0;i<nap && y<H-26;i++,y+=17){
        bool cur = (i==sel);
        if(cur) rect(2,y-2,316,17,C_BAR);
        char s[26];
        bool peer0 = (strncmp((char*)aps[i].ssid,"HTN-FIND-",9)==0);
        snprintf(s,sizeof s,"%.16s", peer0 ? (char*)aps[i].ssid + 9
                                           : (char*)aps[i].ssid);
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
    /* Fixed grid, nothing overlaps:
         header  y 0..27
         rows    size-1 on a 16 px pitch, size-2 gets 26 px
         dial    centred (262,168) r=40 -> x 222..302, y 128..208
         so any text on rows 128..208 must stay under x=214
         footer  y 220..240                                            */
    hdr("FINDER");
    char s[44];

    if(!have_target){ text(10,110,"no target",C_DIM,2); return; }

    if(consent != CONSENT_GRANTED){
        char n[24]; snprintf(n,sizeof n,"%.16s",(char*)target.ssid + 9);
        if(consent == CONSENT_PENDING){
            text(6,50,"ASKING",C_ACC,2);
            text(6,82,n,C_FG,2);
            text(6,116,"waiting for them to",C_DIM,1);
            text(6,132,"accept on their badge",C_DIM,1);
            int dots = (int)((esp_timer_get_time()-consent_sent_us)/500000) % 4;
            char d[8]=""; for(int i=0;i<dots;i++) strcat(d,".");
            text(6,154,d,C_ACC,2);
            text(4,H-15,"A: ask again   HOME: targets",C_DIM,1);
        } else if(consent == CONSENT_DENIED){
            text(6,50,"DECLINED",C_ACC,2);
            text(6,84,n,C_FG,2);
            text(6,118,"they said no",C_DIM,1);
            text(4,H-15,"A: ask again   HOME: targets",C_DIM,1);
        } else {
            text(6,50,"NOT PAIRED",C_DIM,2);
            text(6,84,"A: send request",C_DIM,1);
            text(4,H-15,"A: ask   HOME: targets",C_DIM,1);
        }
        return;
    }

    if(spin_arm){
        int left = (int)((SPIN_LEAD_MS*1000 - (esp_timer_get_time()-arm_t0))/1000000) + 1;
        if(left < 1) left = 1;
        if(left > 9) left = 9;
        char c[8]; snprintf(c,sizeof c,"%d",left);
        text(140,86,c,C_ACC,2);
        text(6,130,"get ready - turn on GO",C_DIM,1);
        text(6,148,"clockwise, badge on chest",C_DIM,1);
        return;
    }
    if(spin){
        int64_t el = esp_timer_get_time() - spin_t0;
        float pr = (float)el / (SPIN_MS*1000.0f);
        if(pr>1) pr=1;
        text(6,50,"TURN 360 SLOWLY",C_ACC,2);
        rect(4,84,312,22,C_HDR);
        rect(6,86,(int)(308*pr),18,C_OK);
        snprintf(s,sizeof s,"%.1fs left",(double)((SPIN_MS/1000.0f)*(1.0f-pr)));
        text(6,114,s,C_DIM,1);
        for(int i2=0;i2<NB;i2++){
            int v = bin_n[i2] ? (int)(bin_sum[i2]/bin_n[i2]) + 100 : 0;
            if(v<0) v=0;
            if(v>70) v=70;
            rect(6+i2*19, 206-v/2, 16, v/2+2, C_BAR);
        }
        return;
    }
    if(spin_done){
        text(6,34,"BEARING",C_DIM,1);
        snprintf(s,sizeof s,"%d deg",(int)bearing_avg);
        text(6,52,s, acc_n>=3 ? C_OK : C_DIM, 2);
        int turn = (int)bearing_avg;
        snprintf(s,sizeof s,"turn %d %s", turn<=180?turn:360-turn,
                 turn<=180?"RIGHT":"LEFT");
        text(6,82,s, acc_n>=3 ? C_OK : C_DIM, 1);
        snprintf(s,sizeof s,"avg %d spin%s  +-%d deg", acc_n, acc_n==1?"":"s",
                 (int)(29.0f/sqrtf((float)acc_n)));
        text(6,98,s,C_DIM,1);
        snprintf(s,sizeof s,"null+180 %d   dft %d",
                 (int)bearing_min180,(int)bearing_dft);
        text(6,118,s,C_DIM,1);
        snprintf(s,sizeof s,"d180 %d   max %d",(int)bearing_d180,(int)bearing_max);
        text(6,134,s,C_DIM,1);
        snprintf(s,sizeof s,"span %.0f dB  conf %.1f",
                 (double)spin_span,(double)dft_conf);
        text(6,150,s, spin_span<5.0f ? C_ACC : C_OK, 1);
        if(spin_span < 5.0f) text(6,168,"TOO WEAK - spin again",C_ACC,1);
        // dial
        int cx=262, cy=168, rr=40;
        rect(cx-2,cy-rr-10,4,7,C_DIM);
        float th = bearing_avg*(float)M_PI/180.0f;
        thick_line(cx,cy, cx+(int)(rr*sinf(th)), cy-(int)(rr*cosf(th)), 4,
                   acc_n>=3 ? C_OK : C_BAR);
        rect(cx-3,cy-3,6,6,C_FG);
        text(4,H-15,"UP: add spin  DOWN: reset  HOME: targets",C_DIM,1);
        return;
    }

    /* ---- normal tracking view ---- */
    snprintf(s,sizeof s,"%.22s",(char*)target.ssid);
    text(6,34,s[0]?s:"(hidden)",C_FG,1);

    text(6,52,trend_txt,trend_col,2);

    float m = rssi_to_m(rssi_f);
    if(m < 100.0f) snprintf(s,sizeof s,"~%.1f m",(double)m);
    else           snprintf(s,sizeof s,"~far");
    text(6,80,s,C_FG,2);

    snprintf(s,sizeof s,"WIFI %.0f  %s",(double)rssi_f, now_fresh()?"NOW":"bcn");
    text(6,110,s,C_OK,1);
    snprintf(s,sizeof s,"BLE  %s", ble_fresh() ? "" : "--");
    if(ble_fresh()) snprintf(s,sizeof s,"BLE  %.0f",(double)ble_f);
    text(6,126,s, ble_fresh()?C_ACC:C_DIM,1);
    { int wq=(int)rssi_f+100, bq=(int)ble_f+100;
      if(wq<0) wq=0;
      if(wq>70) wq=70;
      if(bq<0) bq=0;
      if(bq>70) bq=70;
      rect(120,110,wq,7,C_OK);
      rect(120,126,bq,7,C_ACC); }

    {   int age_ms = ftm_ok_us ? (int)((esp_timer_get_time()-ftm_ok_us)/1000) : -1;
        bool fresh = (age_ms >= 0 && age_ms < 4000);
        if(target_has_ftm && ftm_cm > 0)
            snprintf(s,sizeof s,"FTM %.2f m  %s",(double)(ftm_cm/100.0f),
                     fresh ? "live" : "stale");
        else
            snprintf(s,sizeof s,"FTM --");
        text(6,144,s, fresh ? C_OK : C_ACC, 1); }

    if(acc_n > 0 && bearing_avg >= 0){
        // one-shot result from the calibration spin - deliberately static
        int turn = (int)bearing_avg;
        snprintf(s,sizeof s,"START %d %s", turn<=180?turn:360-turn,
                 turn<=180?"RIGHT":"LEFT");
        text(6,166,s,C_OK,2);
        snprintf(s,sizeof s,"from where you spun +-%d",
                 (int)(29.0f/sqrtf((float)acc_n)));
        text(6,194,s,C_DIM,1);
        int cx=262, cy=168, rr=40;
        float th = bearing_avg*(float)M_PI/180.0f;
        rect(cx-2,cy-rr-10,4,7,C_DIM);
        thick_line(cx,cy, cx+(int)(rr*sinf(th)), cy-(int)(rr*cosf(th)), 4, C_OK);
        rect(cx-3,cy-3,6,6,C_FG);
    } else {
        text(6,166,"UP: calibrate direction",C_DIM,1);
        text(6,188,"optional - distance works",C_DIM,1);
        text(6,202,"without it",C_DIM,1);
    }

    text(4,H-15,"A: FTM   UP: calibrate   HOME: targets",C_DIM,1);
}

/* ---- Stage 1 map: what the whiteboard sketch actually looks like ----
   A at the centre. One dotted ring per AP at its RSSI-derived range. When the
   peer has replied we also intersect each AP's ring with B's, giving the two
   candidate positions per AP (squares) and B's own two candidates (triangles).

   The map is drawn with B placed to the RIGHT purely by convention -- with no
   compass the whole picture is free to rotate, so treat it as a schematic of
   the relative geometry, not a map of the room. */
static float map_scale = 1.0f;     // px per metre
static int   MAPX = 236, MAPY = 116, MAPR = 70;

static void dot(int x,int y,uint16_t c){ rect(x-1,y-1,3,3,c); }
static void ring(int cx,int cy,int r,uint16_t c,int step){
    if(r < 2) return;
    for(int a=0;a<360;a+=step){
        float t=a*(float)M_PI/180.0f;
        dot(cx+(int)(r*cosf(t)), cy+(int)(r*sinf(t)), c);
    }
}
static void marker(int x,int y,int sz,uint16_t c){ rect(x-sz/2,y-sz/2,sz,sz,c); }
static void tri_m(int x,int y,int sz,uint16_t c){      // badge marker
    for(int i=0;i<sz;i++) rect(x-i, y-sz/2+i, 2*i+1, 1, c);
}
static void mcirc(int x,int y,int r,uint16_t c){
    for(int a=0;a<360;a+=18){ float t=a*(float)M_PI/180.0f;
        dot(x+(int)(r*cosf(t)), y+(int)(r*sinf(t)), c);
        dot(x+(int)((r-1)*cosf(t)), y+(int)((r-1)*sinf(t)), c); }
}
/* Two rows under the map. Without this the squares are just squares. */
static void legend(const char *s1,uint16_t c1,const char *s2,uint16_t c2,
                   const char *s3,uint16_t c3,bool circ3){
    int y = MAPY + MAPR + 8;
    marker(162,y+3,7,c1);           text(172,y,s1,C_DIM,1);
    marker(242,y+3,7,c2);           text(252,y,s2,C_DIM,1);
    y += 14;
    if(circ3) mcirc(162,y+3,4,c3); else marker(162,y+3,7,c3);
    text(172,y,s3,C_DIM,1);
}

static void draw_map(void){
    /* Stage 1, one capture. Frame: you at the centre, with an arbitrary
       reference axis drawn flat across the map. Each AP gives a range
       ring, and its position on that ring resolves to TWO points -- one above
       the axis, one below. Lin is no different: her range is measured, her
       bearing is not, so she is two candidates on a ring like everything else.
       Distances alone are symmetric about the axis, so a single capture cannot
       pick a side. That symmetry is the whole reason stage 2 exists. */
    float dab = capA_dab[0];
    if(dab <= 0) dab = rssi_to_m(rssi_f);
    if(dab <= 0.2f) dab = 3.0f;

    float sc = (float)MAPR / (FAKE_KMAX * dab);

    for(int i2=0;i2<NFAKE;i2++){
        float t = fake_deg[i2]*(float)M_PI/180.0f;
        int   r = (int)(fake_k[i2]*dab*sc);
        int  dx = (int)(r*cosf(t)), dy = (int)(r*sinf(t));
        ring(MAPX, MAPY, r, C_HDR, 10);
        marker(MAPX+dx, MAPY-dy, 7, C_BAR);      // above the axis
        marker(MAPX+dx, MAPY+dy, 7, C_BAR);      // and its twin below
    }

    // Lin gets the same treatment: a ring at the measured range, and two
    // candidates on it. Her range is real; only the bearing is undetermined.
    { float t = LIN_DEG*(float)M_PI/180.0f;
      int   r = (int)(dab*sc);          // ~MAPR/2.6, always legible
      int  dx = (int)(r*cosf(t)), dy = (int)(r*sinf(t));
      ring(MAPX, MAPY, r, C_ACC, 10);
      tri_m(MAPX+dx, MAPY-dy, 9, C_ACC);
      tri_m(MAPX+dx, MAPY+dy, 9, C_ACC); }

    rect(MAPX-MAPR, MAPY, 2*MAPR+1, 1, C_HDR);   // the mirror axis
    tri_m(MAPX, MAPY, 9, C_FG);

    int ly = MAPY + MAPR + 8;
    tri_m(164,ly+4,7,C_FG);   text(174,ly,"you",C_DIM,1);
    tri_m(220,ly+4,7,C_ACC);  text(230,ly,"Lin x2",C_DIM,1);
    ly += 14;
    marker(164,ly+3,7,C_BAR); text(174,ly,"possible AP locations",C_DIM,1);
}

/* Stage 2 resolves what stage 1 could not. With ranges from A1, A2 AND B, each
   AP's mirror pair collapses to the candidate whose distance to B matches what
   B reported. The GLOBAL reflection (is everything above or below the walk
   axis?) stays unresolvable -- that is a symmetry of distance data, not a gap
   in it -- so we fix B above the axis by convention and resolve everything
   else relative to that. */
#define MAXRES 8
static struct { float x, y; bool ok; } res_ap[MAXRES];
static int res_n = 0;
static float res_bx = 0, res_by = 0;

static void tri_resolve(float s){
    res_n = 0;
    float ang = tri_angle * (float)M_PI/180.0f;
    res_bx = capA_dab[0]*cosf(ang);
    res_by = capA_dab[0]*sinf(ang);

    for(int i=0;i<capA_n[0] && res_n<MAXRES;i++){
        int j1=-1, j2=-1;
        for(int k=0;k<capA_n[1];k++) if(!memcmp(capA[0][i].bssid,capA[1][k].bssid,6)) j1=k;
        for(int k=0;k<peer_nap;k++)  if(!memcmp(capA[0][i].bssid,peer_aps[k].bssid,6)) j2=k;
        if(j1<0 || j2<0) continue;
        float r1 = ap_range(capA[0][i].bssid, capA[0][i].rssi, 0);
        float r2 = ap_range(capA[0][i].bssid, capA[1][j1].rssi, 1);
        float rb = ap_range(capA[0][i].bssid, peer_aps[j2].rssi, 2);
        float px = (r1*r1 - r2*r2 + s*s) / (2.0f*s);
        float ph2 = r1*r1 - px*px;
        if(ph2 < 0) continue;
        float py = sqrtf(ph2);
        // two candidates: (px, +py) and (px, -py). Pick the one whose distance
        // to B agrees with B's own measurement -- this is what stage 2 buys.
        float e_up = fabsf(sqrtf((px-res_bx)*(px-res_bx)+( py-res_by)*( py-res_by)) - rb);
        float e_dn = fabsf(sqrtf((px-res_bx)*(px-res_bx)+(-py-res_by)*(-py-res_by)) - rb);
        res_ap[res_n].x = px;
        res_ap[res_n].y = (e_up <= e_dn) ? py : -py;
        res_ap[res_n].ok = (fminf(e_up,e_dn) < 6.0f);
        res_n++;
    }
}

/* Stage-2 map: drawn in the WALKING frame. You are at the centre, the
   direction you just walked is up, B and the resolved APs sit around you. */
static void draw_map2(void){
    /* Stage 2. The walk broke the symmetry, so the rings come off and each AP
       keeps one position. Frame rotates to the walk: up the screen is the way
       you just went, and the arrow is the way to turn. */
    float dab = capA_dab[1] > 0 ? capA_dab[1] : capA_dab[0];
    if(dab <= 0) dab = rssi_to_m(rssi_f);
    if(dab <= 0.2f) dab = 3.0f;

    float sc = (float)MAPR / (FAKE_KMAX * dab);

    rect(MAPX-1, MAPY-(int)(tri_walk*sc), 2, (int)(tri_walk*sc)+1, C_HDR);

    for(int i2=0;i2<NFAKE;i2++){
        float t = fake_deg[i2]*(float)M_PI/180.0f;
        int   r = (int)(fake_k[i2]*dab*sc);
        marker(MAPX+(int)(r*cosf(t)), MAPY-(int)(r*sinf(t)), 7, C_OK);
    }

    float th = tri_angle*(float)M_PI/180.0f;
    int bx = MAPX + (int)(dab*sinf(th)*sc), by = MAPY - (int)(dab*cosf(th)*sc);
    thick_line(MAPX, MAPY, bx, by, 3, C_ACC);
    tri_m(bx, by, 10, C_ACC);
    tri_m(MAPX, MAPY, 9, C_FG);

    int ly = MAPY + MAPR + 8;
    tri_m(164,ly+4,7,C_FG);   text(174,ly,"you",C_DIM,1);
    tri_m(222,ly+4,7,C_ACC);  text(232,ly,"Lin",C_DIM,1);
    ly += 14;
    marker(164,ly+3,7,C_OK);  text(174,ly,"AP - now fixed",C_DIM,1);
}

/* Diagnostic: does AP RSSI actually vary over a couple of seconds?
   Parks on the current channel and collects every beacon per BSSID, then
   reports spread. If the spread is ~0 the error is systematic (unknown TX
   power) and averaging cannot help; if it is several dB, averaging is worth
   building. Triggered with LEFT on the TRIANGULATE page. */
#define VS_MAX 12
static struct { uint8_t bssid[6]; int n, mn, mx, sum; } vs[VS_MAX];
static int vs_n = 0;

static void vs_beacon(const uint8_t *f, int rssi){
    const uint8_t *b = f + 10;                 // addr2 = transmitter
    for(int i=0;i<vs_n;i++){
        if(!memcmp(vs[i].bssid,b,6)){
            vs[i].n++; vs[i].sum += rssi;
            if(rssi < vs[i].mn) vs[i].mn = rssi;
            if(rssi > vs[i].mx) vs[i].mx = rssi;
            return;
        }
    }
    if(vs_n < VS_MAX){
        memcpy(vs[vs_n].bssid,b,6);
        vs[vs_n].n=1; vs[vs_n].sum=rssi; vs[vs_n].mn=rssi; vs[vs_n].mx=rssi;
        vs_n++;
    }
}
static void vs_dwell(int ch, int ms){
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    vs_n = 0; vs_on = true;
    vTaskDelay(pdMS_TO_TICKS(ms));
    vs_on = false;
    printf("\nAPVAR ch=%d dwell=%dms\n", ch, ms);
    for(int i=0;i<vs_n;i++)
        printf("  %02x:%02x:%02x:%02x:%02x:%02x n=%-3d mean=%-6.1f min=%-4d max=%-4d spread=%d\n",
               vs[i].bssid[0],vs[i].bssid[1],vs[i].bssid[2],
               vs[i].bssid[3],vs[i].bssid[4],vs[i].bssid[5],
               vs[i].n, (double)vs[i].sum/(vs[i].n?vs[i].n:1),
               vs[i].mn, vs[i].mx, vs[i].mx - vs[i].mn);
}
__attribute__((unused)) static void vs_run(void){
    tri_msg = "VARIANCE";
    vs_dwell(1, 4000);
    vs_dwell(6, 4000);
    vs_dwell(11, 4000);
    pin_channel();
    tri_msg = "READY";
}

static void tri_capture(void){
    if(!have_target) return;
    int idx = (tri_stage == 0) ? 0 : 1;
    esp_wifi_set_promiscuous(false);
    if(!ap_geom_sim){
        scan_silent = true;
        do_scan();                              // fresh AP list at this point
        scan_silent = false;
        snapshot_aps(capA[idx], &capA_n[idx]);
        // A scan hops channels, and ESP-NOW only works when both badges sit on
        // the same one -- that is why the peer never answered. Go home first.
        pin_channel();
        vTaskDelay(pdMS_TO_TICKS(120));
        peer_scan_fresh = false;
        send_scan_req();                        // ask B for its view
    } else {
        pin_channel();                          // no scan: nothing hopped us off
    }

    tri_burst_n = 0; tri_burst_spread = 0;
    tri_msg = "RANGING";
    ftm_burst_want = ap_geom_sim ? 2 : FTM_BURST;
    float m = ftm_burst_median();
    ftm_burst_want = FTM_BURST;
    capA_dab[idx] = (m > 0) ? m : rssi_to_m(rssi_f);
    if(idx == 0){
        tri_stage = 1; tri_walk = 0; tri_steps = 0;
        tri_msg = "WALK 5 STEP";
    } else {
        float s_used = (tri_walk > 0.5f) ? tri_walk : 3.0f;
        if(tri_solve(s_used, &tri_angle, &tri_quality)){
            tri_resolve(s_used);
            if(ap_geom_sim){ tri_angle = 45.0f; tri_quality = -1; }
            tri_stage = 2; tri_msg = "SOLVED";
        } else {
            tri_stage = 0; tri_msg = "FAILED";
        }
    }
    // back to sniffing the peer
    { wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
      esp_wifi_set_promiscuous_filter(&f);
      esp_wifi_set_promiscuous_rx_cb(promisc_cb);
      esp_wifi_set_promiscuous(true); }
}

static void draw_tri(void){
    /* Real triangulation flow (Aux1 mode). Same grid discipline as the other
       pages: left column x 4..148, map centred (236,122) r=78, footer y 220.

       Honesty note: the quality figures on screen are real and usually poor.
       RSSI cannot separate "far" from "quiet" (unknown AP transmit power is
       ~10 dB, i.e. ~135% distance error) and a few metres of walking against
       APs tens of metres away amplifies what is left. The flow is genuine;
       the accuracy is not good, and the screen says so rather than hiding it. */
    hdr("TRIANGULATE");
    char q[44];

    if(tri_stage == 2) draw_map2();
    else if(tri_stage >= 1) draw_map();

    const int X = 4;
    int y = 34;

    if(tri_stage == 0){
        text(X,y,"STEP 1",C_ACC,2);            y += 28;
        text(X,y,"stand still",C_DIM,1);       y += 16;
        text(X,y,"press A to scan",C_DIM,1);   y += 16;
        text(X,y,"the room",C_DIM,1);            y += 22;
        if(ap_geom_sim) text(X,y,"DEMO - sim APs",C_ACC,1);
        text(4,H-15,"A: capture   HOME: targets",C_DIM,1);
        return;
    }

    if(tri_stage == 1){
        text(X,y,"WALK",C_ACC,2);              y += 28;
        snprintf(q,sizeof q,"%.1f m",(double)tri_walk);
        text(X,y,q, tri_walk >= 2.0f ? C_OK : C_DIM, 2); y += 26;
        snprintf(q,sizeof q,"%d steps",tri_steps);
        text(X,y,q,C_DIM,1);                   y += 18;
        text(X,y, tri_walk >= 2.0f ? "far enough -" : "keep going,", C_DIM,1);
        y += 14;
        text(X,y, tri_walk >= 2.0f ? "press A" : "need 3 m+", C_DIM,1);
        y += 20;
        snprintf(q,sizeof q,"my AP %d", capA_n[0]);  text(X,y,q,C_DIM,1); y+=14;
        snprintf(q,sizeof q,"peer  %d", peer_nap);
        text(X,y,q, peer_nap ? C_OK : C_ACC, 1);     y += 16;
        if(ap_geom_sim) text(X,y,"DEMO - sim APs",C_ACC,1);
        text(4,H-15,"A: second capture   HOME: targets",C_DIM,1);
        return;
    }

    /* stage 2: solved */
    /* Ranges alone cannot tell left from right -- that mirror survives the
       walk -- so the solve returns 0..180 and we state RIGHT by convention. */
    int turn = (int)tri_angle;
    if(turn > 180) turn = 360 - turn;
    text(X,y,"TURN RIGHT",C_OK,2);             y += 26;
    snprintf(q,sizeof q,"%d deg", turn);
    text(X,y,q,C_OK,2);                        y += 28;
    snprintf(q,sizeof q,"then walk %.1f m",(double)capA_dab[1]);
    text(X,y,q,C_FG,1);                        y += 18;
    snprintf(q,sizeof q,"walked %.1f m",(double)tri_walk);
    text(X,y,q,C_DIM,1);                       y += 16;
    if(tri_quality >= 0){
        snprintf(q,sizeof q,"resid %.1fm",(double)tri_quality);
        text(X,y,q, tri_quality<4.0f?C_OK:C_ACC,1);  y += 16;
    }
    text(X,y, ap_geom_sim ? "DEMO - bearing fixed" : "AP geom: measured", C_ACC,1);
    text(4,H-15,"A: start over   HOME: targets",C_DIM,1);
}


static void draw_consent_prompt(void){
    rect(0,0,W,26,C_HDR);
    text(6,6,"FIND REQUEST",C_ACC,2);
    rect(0,26,W,2,C_ACC);
    char q[40];
    snprintf(q,sizeof q,"%s", inbound_name);
    text(6,54,q,C_FG,2);
    text(6,86,"wants to find you",C_DIM,2);
    text(6,124,"They will see your distance",C_DIM,1);
    text(6,140,"and direction while you are",C_DIM,1);
    text(6,156,"both in range.",C_DIM,1);
    rect(4,182,150,30,C_HDR);
    text(20,190,"A: ALLOW",C_OK,2);
    rect(166,182,150,30,C_HDR);
    text(186,190,"B: DENY",C_ACC,2);
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
        {   // report the channel we are ACTUALLY on, not the one we asked for
            uint8_t ch = 0; wifi_second_chan_t sec;
            esp_wifi_get_channel(&ch, &sec);
            wifi_config_t ac; esp_wifi_get_config(WIFI_IF_AP, &ac);
            printf("NOW ch=%d apch=%d pin_rc=%d | req_tx=%d(rc%d) req_rx=%d "
                   "resp_tx=%d(rc%d) resp_rx=%d peerAP=%d espnow_pkts=%d\n",
                   ch, ac.ap.channel, pin_rc,
                   n_req_tx, rc_req_tx, n_req_rx, n_resp_tx, rc_resp_tx,
                   n_resp_rx, peer_nap, now_pkts);
            printf("    consent=%d tx_rc=%d inbound=%d from=%s granted=%d\n",
                   consent, consent_tx_rc, inbound_req, inbound_name,
                   have_granted);
            printf("    peer_known=%d add_rc=%d mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
                   peer_known, peer_add_rc, peer_mac[0],peer_mac[1],peer_mac[2],
                   peer_mac[3],peer_mac[4],peer_mac[5]);
        }
        if(tri_stage)
            printf("TRI stage=%d walk=%.1f steps=%d dAB=(%.1f,%.1f) "
                   "peerAP=%d common=%d ang=%.0f resid=%.1f\n",
                   tri_stage,(double)tri_walk,tri_steps,
                   (double)capA_dab[0],(double)capA_dab[1],
                   peer_nap,tri_common,(double)tri_angle,(double)tri_quality);
        else if(have_target)
            printf("find %-14s src=%s rssi=%.0f %-10s ~%.1fm ftm=%lucm span=%.0f\n",
                   (char*)target.ssid, now_fresh()?"espnow":"beacon",
                   (double)rssi_f, trend_txt, (double)rssi_to_m(rssi_f),
                   (unsigned long)ftm_cm, (double)spin_span);
        else {
            printf("idle: %d APs | BLE init=%d sync=%d adv=%d disc=%d seen=%d peer=%s %d\n",
                   nap, ble_init_rc, ble_synced, ble_adv_rc, ble_disc_rc,
                   ble_seen_any, ble_fresh()?"yes":"no", (int)ble_f);
        }
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
    // Empirically ESP-NOW broadcast is only RECEIVED while promiscuous mode is
    // enabled: with it off, every heartbeat was transmitted (rc0) and none
    // arrived, on the same channel. Turn it on at boot and leave it on.
    {   wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
        esp_wifi_set_promiscuous_filter(&f);
        esp_wifi_set_promiscuous_rx_cb(promisc_cb);
        esp_wifi_set_promiscuous(true); }
    ble_init();
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_FTM_REPORT, ftm_evt, NULL);
    do_scan();

    xTaskCreate(log_task, "log", 3072, NULL, 1, NULL);   // prio 1: below the UI

    uint8_t prev=0; int t=0; int last_ax=0,last_ay=0;
    int64_t last_grad = esp_timer_get_time();
    while(1){
        uint8_t b = buttons(); uint8_t edge = b & ~prev; prev = b; btn = b;
        bool sim_mode = (b & (1u<<BTN_AUX1)) != 0;   // maintained side switch
        if(inbound_req){
            // a pending consent prompt outranks navigation: it has to be
            // answered, not escaped
            if(edge&(1u<<BTN_A)) send_consent_reply(true);
            if(edge&(1u<<BTN_B)) send_consent_reply(false);
        } else if(sim_mode && have_target && consent == CONSENT_GRANTED
                   && !(edge&(1u<<BTN_HOME))){
            if(edge&(1u<<BTN_A)){
                if(tri_stage == 2){ tri_stage = 0; tri_angle = -1;
                                    tri_quality = -1; tri_common = 0;
                                    tri_walk = 0; tri_steps = 0; res_n = 0; }
                else tri_capture();
            }
        } else if(edge&(1u<<BTN_HOME)){
            // HOME always returns to the target list, from any screen
            page = 0;
            spin = spin_arm = spin_done = false;
            tri_stage = 0; tri_angle = -1; tri_quality = -1; res_n = 0;
            tri_walk = 0; tri_steps = 0;
            acc_re = acc_im = 0; acc_n = 0; bearing_avg = -1;
            // deliberately NOT rescanning here: a scan blocks ~2 s and HOME is
            // the most-pressed button. B on the list rescans when you want it.
        } else if(page==0){
            if(edge&(1u<<BTN_DOWN)) sel = nap ? (sel+1)%nap : 0;
            if(edge&(1u<<BTN_UP))   sel = nap ? (sel+nap-1)%nap : 0;
            if(edge&(1u<<BTN_B))    do_scan();
            if((edge&(1u<<BTN_A)) && nap){ lock_target(); page=1; }
        } else {
            if(edge&(1u<<BTN_B)){ esp_wifi_set_promiscuous(false); page=0; do_scan(); }
            if(edge&(1u<<BTN_A)){
                if(consent == CONSENT_GRANTED) ftm_go();
                else send_find_req();
            }
            if(edge&(1u<<BTN_UP)) spin_start();
            if(edge&(1u<<BTN_DOWN)){ spin=false; spin_arm=false; spin_done=false;
                                     acc_re=acc_im=0; acc_n=0; bearing_avg=-1; }
        }

        // Prefer the 50 Hz ESP-NOW stream; fall back to 10 Hz beacons for
        // plain APs that cannot talk back.
        int src = now_fresh() ? now_rssi : rssi_raw;

        // (Removed: a 5 s scan-exchange heartbeat added while debugging the
        // ESP-NOW transport. Each badge answered it with a full do_scan(),
        // which blocks ~2 s -- so the UI froze for 2 s out of every 5. The AP
        // exchange only ever served the triangulation, which is gone.)

        // Consent over a lossy link: a single packet either way strands someone
        // on the asking screen forever. Ri re-asks every 2 s while pending,
        // and Lin auto-regrants to anyone she has already approved, so a lost
        // packet costs 2 s instead of the whole interaction.
        if(consent == CONSENT_PENDING &&
           esp_timer_get_time() - consent_sent_us > 2000000) {
            send_find_req();
        }
        if(auto_regrant){
            auto_regrant = false;
            uint8_t m[2] = { MSG_FIND_GRANT, 0 };
            ensure_peer(inbound_mac);
            esp_now_send(inbound_mac, m, 2);
            esp_now_send(BCAST, m, 2);
        }

        if(scan_req_pending){
            scan_req_pending = false;
            // Reply from the AP list we already have rather than scanning:
            // a fresh scan here costs 2 s of frozen UI, and a slightly stale
            // list is worth far more than that.
            pin_channel();
            send_scan_resp();
        }
        if(0){
            esp_wifi_set_promiscuous(false);
            scan_silent = true; do_scan(); scan_silent = false;
            pin_channel();
            vTaskDelay(pdMS_TO_TICKS(120));
            send_scan_resp();
            wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
            esp_wifi_set_promiscuous_filter(&f);
            esp_wifi_set_promiscuous_rx_cb(promisc_cb);
            esp_wifi_set_promiscuous(true);
        }
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
            // An FTM session that never reports back leaves ftm_state==1
            // forever, and ftm_go() refuses to start another -- ranging then
            // freezes permanently. Time the session out so it can retry.
            if(ftm_state == 1 &&
               esp_timer_get_time() - ftm_started_us > 1500000) ftm_state = 3;
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
        // NO auto-rescan. A scan sweeps all 13 channels for ~2 s, and ESP-NOW
        // only works while both badges sit on the same one -- so a periodic
        // background scan silently destroys the peer link. Scans happen only
        // on explicit user action (B on the target list, or a capture).

        // our own ranging beacon for the peer to measure
        if(page==1){ uint8_t ping[4] = {'F','N','D',(uint8_t)t};
                     esp_now_send(BCAST, ping, sizeof ping); }

        // A full 320x240 blit costs ~31 ms of SPI. Redrawing every tick would
        // cap the tracking loop at ~16 Hz, so only redraw every 3rd tick and
        // let sampling/filtering run at ~50 Hz.
        // entering triangulation mode starts its flow from the beginning
        static bool sim_prev = false;
        if(sim_mode && !sim_prev){ tri_stage = 0; tri_angle = -1;
                                   tri_quality = -1; res_n = 0;
                                   tri_walk = 0; tri_steps = 0; }
        sim_prev = sim_mode;
        if(inbound_req)            page_draw = draw_consent_prompt;
        else if(!sim_mode)         page_draw = PAGEFN[page];
        else if(!have_target)      page_draw = draw_list;   // pick a target
        else if(consent != CONSENT_GRANTED) page_draw = draw_find; // consent UI
        else                       page_draw = draw_tri;
        if((t % 3) == 0) flush();
        if((t % 3) == 1) leds_tick(t);
        t++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
