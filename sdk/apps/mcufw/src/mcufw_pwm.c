/*
 * mcufw_pwm.c -- color2 (YLDP06YL) 4-Kanal-RGBW-PWM + GPIO44 Enable (Phase 4b.1).
 *
 * Hardware-Mapping aus color2_142_0027.elf (Ghidra RE):
 *   GPT0 CH0 -> GPIO0  (Rot)
 *   GPT0 CH1 -> GPIO1  (Grün)
 *   GPT0 CH4 -> GPIO4  (Blau)
 *   GPT0 CH5 -> GPIO5  (Weiß)
 *   Periode: 50000 (0xC350), inverted: CMR0 = period - level
 *   GPIO44 (0x2c): active-high LED-Power-Enable
 *
 * CT-Mixing: warm/kalt wird über RGB+W-Anteil gemischt.
 * HSV steuert alle 4 Kanäle getrennt.
 */
#include "mcufw_regs.h"
#include "mcufw_pwm.h"
#include "mcufw_pwm_hal.h"

static mcufw_pwm_stats_t g_pwm;

typedef struct {
	uint8_t  mode;
	uint8_t  bright;
	uint16_t ct;
	uint16_t ch[4]; /* R/G/B/W levels */
} pwm_programmed_t;

static pwm_programmed_t g_prog;
#define PWM_PROG_UNKNOWN 0xFFu

/* --------------------------------------------------------------- logging -- */
static void log_put(char *dst, size_t cap, size_t *pos, const char *s)
{
	while (*s && *pos + 1u < cap)
		dst[(*pos)++] = *s++;
	dst[*pos] = 0;
}

static void log_num(char *dst, size_t cap, size_t *pos, long v)
{
	char tmp[12];
	int n = 0;
	unsigned long u;
	if (v < 0) { log_put(dst, cap, pos, "-"); u = (unsigned long)(-(v+1))+1ul; }
	else { u = (unsigned long)v; }
	do { tmp[n++] = (char)(0+(int)(u%10ul)); u/=10ul; } while(u && n<(int)sizeof(tmp));
	while(n>0 && *pos+1u<cap) dst[(*pos)++]=tmp[--n];
	dst[*pos]=0;
}

static void log_reset(void) { g_pwm.last_log[0]=0; }

/* ---------------------------------------------------- GPT0 channel writes -- */
static void channel_level(uint8_t ch, uint16_t level)
{
	uint32_t cmr;
	if (level > MCUFW_PWM_PERIOD) level = MCUFW_PWM_PERIOD;
	cmr = (uint32_t)MCUFW_PWM_PERIOD - (uint32_t)level;
	mcufw_pwm_hal_write(GPT_CHAN(ch) + GPT_CH_CMR0, cmr);
	mcufw_pwm_hal_write(GPT_USER_REQ, GPT_CMR_UPDT(ch));
	g_pwm.reg_writes += 2u;
}

/* GPIO44 LED-Power-Enable via HAL-Seam (host-testbar). */
extern void mcufw_pwm_hal_gpio_set(uint8_t pin, uint8_t on);

static void led_enable_set(uint8_t on)
{
	mcufw_pwm_hal_gpio_set(MCUFW_PIN_LED_ENABLE, on);
	g_pwm.led_enable = on;
}

static void drive_channels(uint16_t r, uint16_t g, uint16_t b, uint16_t w)
{
	channel_level(MCUFW_PWM_CH_R, r);
	channel_level(MCUFW_PWM_CH_G, g);
	channel_level(MCUFW_PWM_CH_B, b);
	channel_level(MCUFW_PWM_CH_W, w);
	g_pwm.ch_level[0] = r;
	g_pwm.ch_level[1] = g;
	g_pwm.ch_level[2] = b;
	g_pwm.ch_level[3] = w;
	if (r||g||b||w) led_enable_set(1);
	else led_enable_set(0);
}

/* Simple HSV->RGBW conversion for color2.
 * Hue 0..359, Sat 0..100, Val 0..100.
 * Weiß-Anteil = min(R,G,B) wenn Sat<100, sonst 0. */
static void hsv_to_rgbw(uint16_t hue, uint8_t sat, uint8_t val,
                        uint16_t *r, uint16_t *g, uint16_t *b, uint16_t *w)
{
	uint32_t region, remainder, p, q, t, v32, s32;
	uint16_t rl, gl, bl, wl;

	if (val == 0) { *r=*g=*b=*w=0; return; }
	v32 = ((uint32_t)val * MCUFW_PWM_PERIOD) / 100u;
	s32 = (uint32_t)sat;

	if (sat == 0) { *r=*g=*b=*w=(uint16_t)v32; return; }

	region = (uint32_t)hue / 60u;
	remainder = ((uint32_t)hue - (region * 60u)) * 255u / 60u;

	p = (v32 * (255u - s32)) / 255u;
	q = (v32 * (255u - (s32 * remainder) / 255u)) / 255u;
	t = (v32 * (255u - (s32 * (255u - remainder)) / 255u)) / 255u;

	switch(region) {
	case 0:  rl=(uint16_t)v32; gl=(uint16_t)t;   bl=(uint16_t)p;   break;
	case 1:  rl=(uint16_t)q;   gl=(uint16_t)v32; bl=(uint16_t)p;   break;
	case 2:  rl=(uint16_t)p;   gl=(uint16_t)v32; bl=(uint16_t)t;   break;
	case 3:  rl=(uint16_t)p;   gl=(uint16_t)q;   bl=(uint16_t)v32; break;
	case 4:  rl=(uint16_t)t;   gl=(uint16_t)p;   bl=(uint16_t)v32; break;
	default: rl=(uint16_t)v32; gl=(uint16_t)p;   bl=(uint16_t)q;   break;
	}
	/* White channel extraction: minimum of RGB scaled by inverse saturation */
	wl = 0;
	if (sat < 100) {
		uint16_t mn = rl < gl ? (rl < bl ? rl : bl) : (gl < bl ? gl : bl);
		wl = (uint16_t)(((uint32_t)mn * (100u - sat)) / 100u);
	}
	*r=rl; *g=gl; *b=bl; *w=wl;
}

/* CT mixing: simple linear interpolation between warm (2700K) and cool (6500K).
 * ct range: 2700..6500. Brightness 0..100. */
static void ct_to_rgbw(uint8_t bright, uint16_t ct, uint16_t *r, uint16_t *g, uint16_t *b, uint16_t *w)
{
	uint16_t level = mcufw_pwm_duty(bright);
	uint32_t ratio; /* 0=warm, 255=cool */

	if (ct <= 2700) ratio = 0;
	else if (ct >= 6500) ratio = 255;
	else ratio = ((uint32_t)(ct - 2700u) * 255u) / (6500u - 2700u);

	/* Warm: more red+white, less blue. Cool: more blue, less red. */
	*r = (uint16_t)((uint32_t)level * (255u - ratio/3) / 255u);
	*g = level;
	*b = (uint16_t)((uint32_t)level * (ratio/2) / 255u);
	*w = (uint16_t)((uint32_t)level * (255u - ratio/4) / 255u);
}

/* ------------------------------------------------------------- backend ---- */
void mcufw_pwm_init(void)
{
	g_pwm.set_calls=0; g_pwm.hsv_calls=0;
	g_pwm.mode=MCUFW_PWM_MODE_OFF; g_pwm.bright=0;
	g_pwm.ct=0; g_pwm.hue=0; g_pwm.sat=0; g_pwm.val=0;
	g_pwm.powered=0;
	for(int i=0;i<4;i++) g_pwm.ch_level[i]=0;
	g_pwm.led_enable=0;
	g_pwm.restore_mode=MCUFW_PWM_MODE_OFF; g_pwm.restore_bright=0;
	for(int i=0;i<4;i++) g_pwm.restore_ch[i]=0;
	g_pwm.reg_writes=0; g_pwm.dirty=0;
	g_pwm.service_applied=0; g_pwm.service_idle=0;
	g_pwm.skipped_identical=0; g_pwm.last_log[0]=0;
	g_prog.mode=PWM_PROG_UNKNOWN; g_prog.bright=0; g_prog.ct=0;
	for(int i=0;i<4;i++) g_prog.ch[i]=0;
	led_enable_set(0);
}

void mcufw_pwm_invalidate(void) { g_prog.mode=PWM_PROG_UNKNOWN; }

void mcufw_pwm_set(uint8_t bright, uint16_t ct)
{
	size_t pos=0;
	uint16_t r,g,b,w;

	g_pwm.set_calls++; log_reset(); g_pwm.ct=ct;

	if (bright==0) {
		if (g_prog.mode==MCUFW_PWM_MODE_OFF) {
			g_pwm.mode=MCUFW_PWM_MODE_OFF; g_pwm.bright=0; g_pwm.powered=0;
			g_pwm.skipped_identical++;
			log_put(g_pwm.last_log,sizeof(g_pwm.last_log),&pos,"pwm off same");
			return;
		}
		g_pwm.mode=MCUFW_PWM_MODE_OFF; g_pwm.bright=0; g_pwm.powered=0;
		drive_channels(0,0,0,0);
		g_prog.mode=MCUFW_PWM_MODE_OFF; g_prog.bright=0; g_prog.ct=ct;
		for(int i=0;i<4;i++) g_prog.ch[i]=0;
		g_pwm.dirty=1;
		log_put(g_pwm.last_log,sizeof(g_pwm.last_log),&pos,"pwm off");
		return;
	}

	ct_to_rgbw(bright, ct, &r, &g, &b, &w);

	if (g_prog.mode==MCUFW_PWM_MODE_CT && g_prog.bright==bright && g_prog.ct==ct &&
	    g_prog.ch[0]==r && g_prog.ch[1]==g && g_prog.ch[2]==b && g_prog.ch[3]==w) {
		g_pwm.mode=MCUFW_PWM_MODE_CT; g_pwm.bright=bright; g_pwm.powered=1;
		g_pwm.skipped_identical++;
		log_put(g_pwm.last_log,sizeof(g_pwm.last_log),&pos,"pwm ct same");
		return;
	}

	g_pwm.mode=MCUFW_PWM_MODE_CT; g_pwm.bright=bright; g_pwm.powered=1;
	drive_channels(r,g,b,w);
	g_prog.mode=MCUFW_PWM_MODE_CT; g_prog.bright=bright; g_prog.ct=ct;
	g_prog.ch[0]=r; g_prog.ch[1]=g; g_prog.ch[2]=b; g_prog.ch[3]=w;
	g_pwm.dirty=1;
	g_pwm.restore_mode=MCUFW_PWM_MODE_CT; g_pwm.restore_bright=bright;
	for(int i=0;i<4;i++) g_pwm.restore_ch[i]=g_pwm.ch_level[i];
	log_put(g_pwm.last_log,sizeof(g_pwm.last_log),&pos,"pwm ct b=");
	log_num(g_pwm.last_log,sizeof(g_pwm.last_log),&pos,(long)bright);
}

void mcufw_pwm_hsv(uint16_t hue, uint8_t sat, uint8_t val)
{
	size_t pos=0;
	uint16_t r,g,b,w;

	g_pwm.hsv_calls++; log_reset();
	g_pwm.hue=hue; g_pwm.sat=sat; g_pwm.val=val;

	if (val==0) {
		if (g_prog.mode==MCUFW_PWM_MODE_OFF) {
			g_pwm.mode=MCUFW_PWM_MODE_OFF; g_pwm.bright=0; g_pwm.powered=0;
			g_pwm.skipped_identical++;
			log_put(g_pwm.last_log,sizeof(g_pwm.last_log),&pos,"pwm hsv v=0 same");
			return;
		}
		g_pwm.mode=MCUFW_PWM_MODE_OFF; g_pwm.bright=0; g_pwm.powered=0;
		drive_channels(0,0,0,0);
		g_prog.mode=MCUFW_PWM_MODE_OFF; g_prog.bright=0; g_prog.ct=0;
		for(int i=0;i<4;i++) g_prog.ch[i]=0;
		g_pwm.dirty=1;
		log_put(g_pwm.last_log,sizeof(g_pwm.last_log),&pos,"pwm hsv v=0");
		return;
	}

	hsv_to_rgbw(hue, sat, val, &r, &g, &b, &w);

	if (g_prog.mode==MCUFW_PWM_MODE_HSV && g_prog.bright==val &&
	    g_prog.ch[0]==r && g_prog.ch[1]==g && g_prog.ch[2]==b && g_prog.ch[3]==w) {
		g_pwm.mode=MCUFW_PWM_MODE_HSV; g_pwm.bright=val; g_pwm.powered=1;
		g_pwm.skipped_identical++;
		log_put(g_pwm.last_log,sizeof(g_pwm.last_log),&pos,"pwm hsv same");
		return;
	}

	g_pwm.mode=MCUFW_PWM_MODE_HSV; g_pwm.bright=val; g_pwm.powered=1;
	drive_channels(r,g,b,w);
	g_prog.mode=MCUFW_PWM_MODE_HSV; g_prog.bright=val; g_prog.ct=0;
	g_prog.ch[0]=r; g_prog.ch[1]=g; g_prog.ch[2]=b; g_prog.ch[3]=w;
	g_pwm.dirty=1;
	g_pwm.restore_mode=MCUFW_PWM_MODE_HSV; g_pwm.restore_bright=val;
	for(int i=0;i<4;i++) g_pwm.restore_ch[i]=g_pwm.ch_level[i];
	log_put(g_pwm.last_log,sizeof(g_pwm.last_log),&pos,"pwm hsv h=");
	log_num(g_pwm.last_log,sizeof(g_pwm.last_log),&pos,(long)hue);
}

void mcufw_pwm_restore(void)
{
	if (g_pwm.restore_ch[0]==0 && g_pwm.restore_ch[1]==0 &&
	    g_pwm.restore_ch[2]==0 && g_pwm.restore_ch[3]==0) return;

	g_pwm.mode=g_pwm.restore_mode; g_pwm.bright=g_pwm.restore_bright;
	drive_channels(g_pwm.restore_ch[0], g_pwm.restore_ch[1],
	               g_pwm.restore_ch[2], g_pwm.restore_ch[3]);
	g_prog.mode=g_pwm.restore_mode; g_prog.bright=g_pwm.restore_bright;
	g_prog.ct=g_pwm.ct;
	for(int i=0;i<4;i++) g_prog.ch[i]=g_pwm.restore_ch[i];
	g_pwm.powered=1; g_pwm.dirty=1;
}

int mcufw_pwm_service(void)
{
	if (!g_pwm.dirty) { g_pwm.service_idle++; return 0; }
	g_pwm.dirty=0; g_pwm.service_applied++; return 1;
}

uint32_t mcufw_pwm_cmr0(uint8_t ch)
{
	return mcufw_pwm_hal_read(GPT_CHAN(ch)+GPT_CH_CMR0);
}

const mcufw_pwm_stats_t *mcufw_pwm_stats(void) { return &g_pwm; }
void mcufw_pwm_stats_reset(void) { mcufw_pwm_init(); }

uint16_t mcufw_pwm_channel_level(uint8_t ch_idx)
{
	if (ch_idx>=4) return 0;
	return g_pwm.ch_level[ch_idx];
}

uint16_t mcufw_pwm_duty(uint8_t bright)
{
	if (bright==0) return 0u;
	if (bright>=100u) return MCUFW_PWM_PERIOD;
	return (uint16_t)(((uint32_t)bright * MCUFW_PWM_PERIOD)/100u);
}
