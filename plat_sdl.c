#include <SDL/SDL.h>
#include <unistd.h>
#include <sys/time.h>
#include <directfb.h>
#include "core.h"
#include "libpicofe/fonts.h"
#include "libpicofe/plat.h"
#include "menu.h"
#include "plat.h"
#include "scale.h"
#include "util.h"

#define PICOARCH_DISABLE_AUDIO 0

static SDL_Surface* screen;
static IDirectFB            *dfb       = NULL;
static IDirectFBSurface     *primary   = NULL;
static IDirectFBSurface     *source    = NULL;
static int                   dfb_w = 0, dfb_h = 0;
static int                   source_surf_w = 0, source_surf_h = 0;
static int                   source_w = 0, source_h = 0;

struct audio_state {
	unsigned buf_w;
	unsigned max_buf_w;
	unsigned buf_r;
	size_t buf_len;
	struct audio_frame *buf;
	int in_sample_rate;
	int out_sample_rate;
	int sample_rate_adj;
	int adj_out_sample_rate;
};

struct audio_state audio;

static void plat_sound_select_resampler(void);
void (*plat_sound_write)(const struct audio_frame *data, int frames);

#define DRC_MAX_ADJUSTMENT 0.003
#define DRC_ADJ_BELOW 40
#define DRC_ADJ_ABOVE 60

static char msg[HUD_LEN];
static unsigned msg_priority = 0;
static unsigned msg_expire = 0;

static bool frame_dirty = false;
static int frame_time = 1000000 / 60;

static uint64_t plat_get_ticks_us_u64(void) {
	uint64_t ret;
	struct timeval tv;

	gettimeofday(&tv, NULL);

	ret = (uint64_t)tv.tv_sec * 1000000;
	ret += (uint64_t)tv.tv_usec;

	return ret;
}

/* ------------------------------------------------------------------ */
/*  DFB source surface 管理：按需重建                                  */
/* ------------------------------------------------------------------ */
static void ensure_source_size(int w, int h)
{
	if (w == source_surf_w && h == source_surf_h)
		return;

	if (source) {
		source->Release(source);
		source = NULL;
	}

	DFBSurfaceDescription sdsc = {
		.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT,
		.width       = w,
		.height      = h,
		.pixelformat = DSPF_RGB16,
	};

	if (dfb->CreateSurface(dfb, &sdsc, &source) != DFB_OK) {
		PA_ERROR("ensure_source_size: CreateSurface %dx%d failed\n", w, h);
		source = NULL;
		source_surf_w = source_surf_h = 0;
		return;
	}

	source_surf_w = w;
	source_surf_h = h;
	PA_INFO("source surface resized to %dx%d\n", w, h);
}

/* ------------------------------------------------------------------ */
/*  HUD 消息                                                           */
/* ------------------------------------------------------------------ */
static void video_expire_msg(void)
{
	msg[0] = '\0';
	msg_priority = 0;
	msg_expire = 0;
}

static void video_update_msg(void)
{
	if (msg[0] && msg_expire < plat_get_ticks_ms())
		video_expire_msg();
}

static void video_clear_msg(uint16_t *dst, uint32_t h, uint32_t pitch)
{
	memset(dst + (h - 10) * pitch, 0, 10 * pitch * sizeof(uint16_t));
}

static void video_print_msg(uint16_t *dst, uint32_t h, uint32_t pitch, char *msg)
{
	basic_text_out16_nf(dst, pitch, 2, h - 10, msg);
}

/* ------------------------------------------------------------------ */
/*  fb_flip：只做 StretchBlit + Flip，source 已由调用方填好              */
/* ------------------------------------------------------------------ */
static void fb_blit(void)
{
	if (!source || !primary || source_w == 0 || source_h == 0)
		return;

	DFBRectangle sr = { 0, 0, source_w, source_h };
	DFBRectangle dr = { 0, 0, dfb_w, dfb_h };
	primary->StretchBlit(primary, source, &sr, &dr);
	primary->Flip(primary, NULL, DSFLIP_NONE);
}

/*
 * 菜单专用：把 screen(SDL surface) 内容拷进 source，再 blit 到屏幕。
 * 菜单分辨率固定为 SCREEN_WIDTH x SCREEN_HEIGHT。
 */
static void *fb_flip_menu(void)
{
	if (!screen)
		return NULL;

	ensure_source_size(SCREEN_WIDTH, SCREEN_HEIGHT);
	source_w = SCREEN_WIDTH;
	source_h = SCREEN_HEIGHT;

	void *sptr;
	int spitch;
	if (source && source->Lock(source, DSLF_WRITE, &sptr, &spitch) == DFB_OK) {
		const uint8_t *src = (const uint8_t *)screen->pixels;
		uint8_t       *dst = (uint8_t *)sptr;
		int row = SCREEN_WIDTH * 2;

		if (spitch == row && screen->pitch == row) {
			memcpy(dst, src, row * SCREEN_HEIGHT);
		} else {
			for (int y = 0; y < SCREEN_HEIGHT; y++)
				memcpy(dst + y * spitch, src + y * screen->pitch, row);
		}
		source->Unlock(source);
	}

	fb_blit();
	return screen->pixels;
}

/* ------------------------------------------------------------------ */
/*  音频：空写入（调试用，可恢复）                                      */
/* ------------------------------------------------------------------ */
static int audio_resample_passthrough(struct audio_frame data) {
	audio.buf[audio.buf_w++] = data;
	if (audio.buf_w >= audio.buf_len) audio.buf_w = 0;

	return 1;
}

static int audio_resample_nearest(struct audio_frame data) {
	static int diff = 0;
	int consumed = 0;

	if (diff < audio.adj_out_sample_rate) {
		audio.buf[audio.buf_w++] = data;
		if (audio.buf_w >= audio.buf_len) audio.buf_w = 0;

		diff += audio.in_sample_rate;
	}

	if (diff >= audio.adj_out_sample_rate) {
		consumed++;
		diff -= audio.adj_out_sample_rate;
	}

	return consumed;
}

/* 空写入：丢弃所有音频帧 */
void plat_sound_write_null(const struct audio_frame *data, int frames)
{
	(void)data;
	(void)frames;
}

/* ------------------------------------------------------------------ */
/*  截图 / 读图（保持原有逻辑，使用 screen surface）                    */
/* ------------------------------------------------------------------ */
void *plat_prepare_screenshot(int *w, int *h, int *bpp)
{
	if (w) *w = SCREEN_WIDTH;
	if (h) *h = SCREEN_HEIGHT;
	if (bpp) *bpp = SCREEN_BPP;

	return screen->pixels;
}

int plat_dump_screen(const char *filename) {
	char imgname[MAX_PATH];
	int ret = -1;
	SDL_Surface *surface = NULL;

	snprintf(imgname, MAX_PATH, "%s.bmp", filename);

	if (g_menuscreen_ptr) {
		surface = SDL_CreateRGBSurfaceFrom(g_menubg_src_ptr,
		                                   g_menubg_src_w,
		                                   g_menubg_src_h,
		                                   16,
		                                   g_menubg_src_w * sizeof(uint16_t),
		                                   0xF800, 0x07E0, 0x001F, 0x0000);
		if (surface) {
			ret = SDL_SaveBMP(surface, imgname);
			SDL_FreeSurface(surface);
		}
	} else {
		ret = SDL_SaveBMP(screen, imgname);
	}

	return ret;
}

int plat_load_screen(const char *filename, void *buf, size_t buf_size, int *w, int *h, int *bpp) {
	int ret = -1;
	char imgname[MAX_PATH];
	SDL_Surface *imgsurface = NULL;
	SDL_Surface *surface = NULL;

	snprintf(imgname, MAX_PATH, "%s.bmp", filename);
	imgsurface = SDL_LoadBMP(imgname);
	if (!imgsurface)
		goto finish;

	surface = SDL_DisplayFormat(imgsurface);
	if (!surface)
		goto finish;

	if (surface->pitch > SCREEN_PITCH ||
	    surface->h > SCREEN_HEIGHT ||
	    surface->w == 0 ||
	    surface->h * surface->pitch > buf_size)
		goto finish;

	memcpy(buf, surface->pixels, surface->pitch * surface->h);
	*w = surface->w;
	*h = surface->h;
	*bpp = surface->pitch / surface->w;

	ret = 0;

finish:
	if (imgsurface)
		SDL_FreeSurface(imgsurface);
	if (surface)
		SDL_FreeSurface(surface);
	return ret;
}

/* ------------------------------------------------------------------ */
/*  菜单视频接口：走 screen(SDL) → fb_flip_menu 路径                   */
/* ------------------------------------------------------------------ */
void plat_video_menu_enter(int is_rom_loaded)
{
	if (g_menuscreen_ptr)
		return;

	SDL_LockSurface(screen);
	memcpy(g_menubg_src_ptr, screen->pixels, g_menubg_src_h * g_menubg_src_pp * sizeof(uint16_t));
	SDL_UnlockSurface(screen);
	g_menuscreen_ptr = fb_flip_menu();
}

void plat_video_menu_begin(void)
{
	SDL_LockSurface(screen);
	menu_begin();
}

void plat_video_menu_end(void)
{
	menu_end();
	SDL_UnlockSurface(screen);
	g_menuscreen_ptr = fb_flip_menu();
}

void plat_video_menu_leave(void)
{
	memset(g_menubg_src_ptr, 0, g_menuscreen_h * g_menuscreen_pp * sizeof(uint16_t));

	SDL_LockSurface(screen);
	memset(screen->pixels, 0, g_menuscreen_h * g_menuscreen_pp * sizeof(uint16_t));
	SDL_UnlockSurface(screen);
	fb_flip_menu();
	SDL_LockSurface(screen);
	memset(screen->pixels, 0, g_menuscreen_h * g_menuscreen_pp * sizeof(uint16_t));
	SDL_UnlockSurface(screen);

	g_menuscreen_ptr = NULL;
}

void plat_video_open(void)
{
}

void plat_video_set_msg(const char *new_msg, unsigned priority, unsigned msec)
{
	if (!new_msg) {
		video_expire_msg();
	} else if (priority >= msg_priority) {
		snprintf(msg, HUD_LEN, "%s", new_msg);
		string_truncate(msg, HUD_LEN - 1);
		msg_priority = priority;
		msg_expire = plat_get_ticks_ms() + msec;
	}
}

/* ------------------------------------------------------------------ */
/*  游戏帧处理：核心帧直写 DFB source，GE 硬件做缩放                   */
/* ------------------------------------------------------------------ */
void plat_video_process(const void *data, unsigned width, unsigned height, size_t pitch) {
	frame_dirty = true;

	ensure_source_size(width, height);
	source_w = width;
	source_h = height;

	if (source) {
		void *sptr;
		int spitch;

		if (source->Lock(source, DSLF_WRITE, &sptr, &spitch) == DFB_OK) {
			const uint8_t *src = (const uint8_t *)data;
			uint8_t       *dst = (uint8_t *)sptr;
			int row = width * 2;

			if ((int)pitch == row && spitch == row) {
				memcpy(dst, src, row * height);
			} else {
				for (unsigned y = 0; y < height; y++)
					memcpy(dst + y * spitch, src + y * pitch, row);
			}

			if (msg[0]) {
				uint16_t *dst16 = (uint16_t *)sptr;
				int pitch16 = spitch / 2;
				video_print_msg(dst16, height, pitch16, msg);
			}

			source->Unlock(source);
		}
	}

	video_update_msg();
}

void plat_video_flip(void)
{
	static uint64_t next_frame_time_us = 0;

	if (frame_dirty) {
		if (enable_drc) {
			uint64_t time = plat_get_ticks_us_u64();

			if (limit_frames && time < next_frame_time_us) {
				uint32_t delaytime = (next_frame_time_us - time - 1) / 1000 + 1;

				if (delaytime < 1000)
					SDL_Delay(delaytime);
				else
					next_frame_time_us = 0;

				time = plat_get_ticks_us_u64();
			}

			if (!next_frame_time_us || !limit_frames) {
				next_frame_time_us = time;
			}

			fb_blit();

			do {
				next_frame_time_us += frame_time;
			} while (next_frame_time_us < time);
		} else {
			fb_blit();
			next_frame_time_us = 0;
		}

		frame_dirty = false;
	}
}

void plat_video_close(void)
{
}

unsigned plat_cpu_ticks(void)
{
	long unsigned ticks = 0;
	long ticksps = 0;
	FILE *file = NULL;

	file = fopen("/proc/self/stat", "r");
	if (!file)
		goto finish;

	if (!fscanf(file, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu", &ticks))
		goto finish;

	ticksps = sysconf(_SC_CLK_TCK);

	if (ticksps)
		ticks = ticks * 100 / ticksps;

finish:
	if (file)
		fclose(file);

	return ticks;
}

static void plat_sound_callback(void *unused, uint8_t *stream, int len)
{
	int16_t *p = (int16_t *)stream;
	if (audio.buf_len == 0)
		return;

	len /= (sizeof(int16_t) * 2);

	while (audio.buf_r != audio.buf_w && len > 0) {
		*p++ = audio.buf[audio.buf_r].left;
		*p++ = audio.buf[audio.buf_r].right;
		audio.max_buf_w = audio.buf_r;

		len--;
		audio.buf_r++;

		if (audio.buf_r >= audio.buf_len) audio.buf_r = 0;
	}

	while(len > 0) {
		*p++ = 0;
		--len;
	}
}

static void plat_sound_finish(void)
{
	SDL_PauseAudio(1);
	SDL_CloseAudio();
	if (audio.buf) {
		free(audio.buf);
		audio.buf = NULL;
	}
}

static int plat_sound_init(void)
{
	if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		return -1;
	}

	SDL_AudioSpec spec, received;

	spec.freq = MIN(sample_rate, MAX_SAMPLE_RATE);
	spec.format = AUDIO_S16;
	spec.channels = 2;
	spec.samples = 512;
	spec.callback = plat_sound_callback;

	if (SDL_OpenAudio(&spec, &received) < 0) {
		plat_sound_finish();
		return -1;
	}

	audio.in_sample_rate = sample_rate;
	audio.out_sample_rate = received.freq;
	audio.sample_rate_adj = audio.out_sample_rate * DRC_MAX_ADJUSTMENT;
	audio.adj_out_sample_rate = audio.out_sample_rate;

	plat_sound_select_resampler();
	plat_sound_resize_buffer();

	SDL_PauseAudio(0);
	return 0;
}

int plat_sound_occupancy(void)
{
	int buffered = 0;
	if (audio.buf_len == 0)
		return 0;

	if (audio.buf_w != audio.buf_r) {
		buffered = audio.buf_w > audio.buf_r ?
			audio.buf_w - audio.buf_r :
			(audio.buf_w + audio.buf_len) - audio.buf_r;
	}

	return buffered * 100 / audio.buf_len;
}

#define BATCH_SIZE 100
void plat_sound_write_resample(const struct audio_frame *data, int frames, int (*resample)(struct audio_frame data), bool drc)
{
	int consumed = 0;
	if (audio.buf_len == 0)
		return;

	if (drc) {
		int occupancy = plat_sound_occupancy();

		if (occupancy < DRC_ADJ_BELOW) {
			audio.adj_out_sample_rate = audio.out_sample_rate + audio.sample_rate_adj;
		} else if (occupancy > DRC_ADJ_ABOVE) {
			audio.adj_out_sample_rate = audio.out_sample_rate - audio.sample_rate_adj;
		} else {
			audio.adj_out_sample_rate = audio.out_sample_rate;
		}
	}

	SDL_LockAudio();

	while (frames > 0) {
		int tries = 0;
		int amount = MIN(BATCH_SIZE, frames);

		while (tries < 10 && audio.buf_w == audio.max_buf_w) {
			tries++;
			SDL_UnlockAudio();

			if (!limit_frames)
				return;

			plat_sleep_ms(1);
			SDL_LockAudio();
		}

		while (amount && audio.buf_w != audio.max_buf_w) {
			consumed = resample(*data);
			data += consumed;
			amount -= consumed;
			frames -= consumed;
		}
	}
	SDL_UnlockAudio();
}

void plat_sound_write_passthrough(const struct audio_frame *data, int frames)
{
	plat_sound_write_resample(data, frames, audio_resample_passthrough, false);
}

void plat_sound_write_nearest(const struct audio_frame *data, int frames)
{
	plat_sound_write_resample(data, frames, audio_resample_nearest, false);
}

void plat_sound_write_drc(const struct audio_frame *data, int frames)
{
	plat_sound_write_resample(data, frames, audio_resample_nearest, true);
}

void plat_sound_resize_buffer(void) {
	size_t buf_size;
	SDL_LockAudio();

	audio.buf_len = frame_rate > 0
		? current_audio_buffer_size * audio.in_sample_rate / frame_rate
		: 0;

	if (enable_drc)
		audio.buf_len *= 2;

	if (audio.buf_len == 0) {
		SDL_UnlockAudio();
		return;
	}

	buf_size = audio.buf_len * sizeof(struct audio_frame);
	audio.buf = realloc(audio.buf, buf_size);

	if (!audio.buf) {
		SDL_UnlockAudio();
		PA_ERROR("Error initializing sound buffer\n");
		plat_sound_finish();
		return;
	}

	memset(audio.buf, 0, buf_size);
	audio.buf_w = 0;
	audio.buf_r = 0;
	audio.max_buf_w = audio.buf_len - 1;
	SDL_UnlockAudio();
}

static void plat_sound_select_resampler(void)
{
	if (enable_drc) {
		PA_INFO("Using audio adjustment (in: %d, out: %d-%d)\n", audio.in_sample_rate, audio.out_sample_rate - audio.sample_rate_adj, audio.out_sample_rate + audio.sample_rate_adj);
		plat_sound_write = plat_sound_write_drc;
	} else if (audio.in_sample_rate == audio.out_sample_rate) {
		PA_INFO("Using passthrough resampler (in: %d, out: %d)\n", audio.in_sample_rate, audio.out_sample_rate);
		plat_sound_write = plat_sound_write_passthrough;
	} else {
		PA_INFO("Using nearest resampler (in: %d, out: %d)\n", audio.in_sample_rate, audio.out_sample_rate);
		plat_sound_write = plat_sound_write_nearest;
	}
}

void plat_sdl_event_handler(void *event_)
{
}

int plat_init(void)
{
	plat_sound_write = plat_sound_write_nearest;

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
		PA_ERROR("SDL_Init: %s\n", SDL_GetError());
		return -1;
	}
	SDL_ShowCursor(0);

	/* DirectFB 初始化 */
	int dargc = 0; char **dargv = NULL;
	if (DirectFBInit(&dargc, &dargv) != DFB_OK) {
		PA_ERROR("DirectFBInit failed\n"); return -1;
	}
	if (DirectFBCreate(&dfb) != DFB_OK) {
		PA_ERROR("DirectFBCreate failed\n"); return -1;
	}
	dfb->SetCooperativeLevel(dfb, DFSCL_FULLSCREEN);

	/* primary surface */
	DFBSurfaceDescription pdsc = {
		.flags = DSDESC_CAPS,
		.caps  = DSCAPS_PRIMARY | DSCAPS_FLIPPING,
	};
	if (dfb->CreateSurface(dfb, &pdsc, &primary) != DFB_OK) {
		PA_ERROR("primary CreateSurface failed\n"); return -1;
	}
	primary->GetSize(primary, &dfb_w, &dfb_h);
	primary->Clear(primary, 0, 0, 0, 0xFF);
	primary->Flip(primary, NULL, DSFLIP_NONE);

	/* source surface 初始大小 = SCREEN，后续 ensure_source_size 按需调整 */
	ensure_source_size(SCREEN_WIDTH, SCREEN_HEIGHT);
	source_w = SCREEN_WIDTH;
	source_h = SCREEN_HEIGHT;

	/* screen: SDL software surface 作为菜单画布 + 截图用 */
	screen = SDL_CreateRGBSurface(SDL_SWSURFACE,
	                              SCREEN_WIDTH, SCREEN_HEIGHT, 16,
	                              0xF800, 0x07E0, 0x001F, 0x0000);
	if (!screen) {
		PA_ERROR("CreateRGBSurface fail: %s\n", SDL_GetError());
		return -1;
	}

	PA_INFO("DFB primary %dx%d, screen buf %dx%d bpp16 pitch %d\n",
	        dfb_w, dfb_h, screen->w, screen->h, screen->pitch);

	g_menuscreen_w  = SCREEN_WIDTH;
	g_menuscreen_h  = SCREEN_HEIGHT;
	g_menuscreen_pp = SCREEN_WIDTH;
	g_menuscreen_ptr = NULL;

	g_menubg_src_w  = SCREEN_WIDTH;
	g_menubg_src_h  = SCREEN_HEIGHT;
	g_menubg_src_pp = SCREEN_WIDTH;

	if (in_sdl_init(&in_sdl_platform_data, plat_sdl_event_handler)) {
		PA_ERROR("SDL input failed to init: %s\n", SDL_GetError());
		return -1;
	}
	in_probe();

#if PICOARCH_DISABLE_AUDIO
	plat_sound_write = plat_sound_write_null;
	PA_INFO("Audio disabled (PICOARCH_DISABLE_AUDIO=1)\n");
#else
	if (plat_sound_init()) {
		PA_ERROR("SDL sound failed to init: %s\n", SDL_GetError());
		return -1;
	}
#endif

	return 0;
}

int plat_reinit(void)
{
#if !PICOARCH_DISABLE_AUDIO
	if (sample_rate && sample_rate != audio.in_sample_rate) {
		plat_sound_finish();

		if (plat_sound_init()) {
			PA_ERROR("SDL sound failed to init: %s\n", SDL_GetError());
			return -1;
		}
	} else {
		plat_sound_resize_buffer();
		plat_sound_select_resampler();
	}
#endif

	if (frame_rate != 0)
		frame_time = 1000000 / frame_rate;

	scale_update_scaler();
	return 0;
}

void plat_finish(void)
{
#if !PICOARCH_DISABLE_AUDIO
	plat_sound_finish();
#endif

	if (source)  { source->Release(source);  source  = NULL; }
	if (primary) { primary->Release(primary); primary = NULL; }
	if (dfb)     { dfb->Release(dfb);        dfb     = NULL; }

	source_surf_w = source_surf_h = 0;
	source_w = source_h = 0;

	if (screen)  { SDL_FreeSurface(screen);  screen  = NULL; }
	SDL_Quit();
}
