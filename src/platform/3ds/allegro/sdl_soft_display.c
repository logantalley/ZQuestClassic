/*         ______   ___    ___
 *        /\  _  \ /\_ \  /\_ \
 *        \ \ \L\ \\//\ \ \//\ \      __     __   _ __   ___
 *         \ \  __ \ \ \ \  \ \ \   /'__`\ /'_ `\/\`'__\/ __`\
 *          \ \ \/\ \ \_\ \_ \_\ \_/\  __//\ \L\ \ \ \//\ \L\ \
 *           \ \_\ \_\/\____\/\____\ \____\ \____ \ \_\\ \____/
 *            \/_/\/_/\/____/\/____/\/____/\/___L\ \/_/ \/___/
 *                                           /\____/
 *                                           \_/__/
 *
 *      SDL software display, for platforms without OpenGL (3DS).
 *
 *      Replaces src/sdl/sdl_display.c. The display has no video bitmaps:
 *      create_bitmap is NULL, so every bitmap is a memory bitmap. The
 *      backbuffer is a memory bitmap in the window surface's pixel format,
 *      owned by the display (its _display field is set, which makes
 *      al_set_target_bitmap() make the display current). Flipping copies it
 *      into the SDL window surface.
 *
 *      See LICENSE.txt for copyright information.
 */
#include <string.h>

#include "allegro5/allegro.h"
#include "allegro5/internal/aintern.h"
#include "allegro5/internal/aintern_bitmap.h"
#include "allegro5/internal/aintern_display.h"
#include "allegro5/internal/aintern_system.h"
#include "allegro5/internal/aintern_vector.h"
#include "allegro5/platform/allegro_internal_sdl.h"

ALLEGRO_DEBUG_CHANNEL("display")

typedef struct ALLEGRO_DISPLAY_SDL_SOFT
{
   ALLEGRO_DISPLAY_SDL sdl; /* This must be the first member. */
   ALLEGRO_BITMAP *backbuffer;
   Uint32 sdl_format; /* The backbuffer's pixel format, in SDL terms. */
} ALLEGRO_DISPLAY_SDL_SOFT;

static ALLEGRO_DISPLAY_INTERFACE *vt;

float _al_sdl_get_display_pixel_ratio(ALLEGRO_DISPLAY *display)
{
   (void)display;
   return 1.0f;
}

ALLEGRO_DISPLAY *_al_sdl_find_display(uint32_t window_id)
{
   unsigned int i;
   ALLEGRO_SYSTEM *s = al_get_system_driver();
   for (i = 0; i < _al_vector_size(&s->displays); i++) {
      ALLEGRO_DISPLAY_SDL **d = _al_vector_ref(&s->displays, i);
      if (SDL_GetWindowID((*d)->window) == window_id)
         return &(*d)->display;
   }
   return NULL;
}

void _al_sdl_display_event(SDL_Event *e)
{
   ALLEGRO_SYSTEM *s = al_get_system_driver();
   ALLEGRO_DISPLAY *d = NULL;
   ALLEGRO_EVENT event;
   memset(&event, 0, sizeof event);
   event.display.timestamp = al_get_time();

   if (e->type == SDL_WINDOWEVENT) {
      d = _al_sdl_find_display(e->window.windowID);
      switch (e->window.event) {
         case SDL_WINDOWEVENT_FOCUS_GAINED:
            event.display.type = ALLEGRO_EVENT_DISPLAY_SWITCH_IN;
            break;
         case SDL_WINDOWEVENT_FOCUS_LOST:
            event.display.type = ALLEGRO_EVENT_DISPLAY_SWITCH_OUT;
            break;
         case SDL_WINDOWEVENT_CLOSE:
            event.display.type = ALLEGRO_EVENT_DISPLAY_CLOSE;
            break;
         default:
            /* The window always covers the screen; ignore resizes. */
            return;
      }
   }
   else if (e->type == SDL_QUIT) {
      /* aptMainLoop() returned false: the system asked us to quit. */
      event.display.type = ALLEGRO_EVENT_DISPLAY_CLOSE;
      if (_al_vector_size(&s->displays) > 0)
         d = *(ALLEGRO_DISPLAY **)_al_vector_ref(&s->displays, 0);
   }

   if (!d)
      return;
   _al_event_source_lock(&d->es);
   _al_event_source_emit_event(&d->es, &event);
   _al_event_source_unlock(&d->es);
}

static ALLEGRO_DISPLAY *soft_create_display_locked(int w, int h)
{
   ALLEGRO_DISPLAY_SDL_SOFT *soft;
   ALLEGRO_DISPLAY *d;
   ALLEGRO_DISPLAY **add;
   SDL_DisplayMode mode;
   SDL_Surface *surface;
   int adapter = al_get_new_display_adapter();
   int format;

   if (adapter < 0)
      adapter = 0;

   /* The window always covers the whole screen (400x240 on the 3DS top
    * screen); the requested size is only a fallback.
    */
   if (SDL_GetCurrentDisplayMode(adapter, &mode) == 0) {
      w = mode.w;
      h = mode.h;
   }
   else {
      mode.refresh_rate = 0;
   }

   soft = al_calloc(1, sizeof *soft);
   d = (void *)soft;
   d->w = w;
   d->h = h;
   d->refresh_rate = mode.refresh_rate;
   /* No GL, no shaders, no resizing. ALLEGRO_PROGRAMMABLE_PIPELINE in
    * particular must go: al_create_display() would fail to build the
    * default shader.
    */
   d->flags = ALLEGRO_FULLSCREEN_WINDOW | ALLEGRO_FRAMELESS;

   soft->sdl.window = SDL_CreateWindow(al_get_new_window_title(),
      SDL_WINDOWPOS_UNDEFINED_DISPLAY(adapter),
      SDL_WINDOWPOS_UNDEFINED_DISPLAY(adapter), w, h, SDL_WINDOW_BORDERLESS);
   if (!soft->sdl.window) {
      ALLEGRO_ERROR("SDL_CreateWindow failed: %s\n", SDL_GetError());
      al_free(soft);
      return NULL;
   }

   surface = SDL_GetWindowSurface(soft->sdl.window);
   if (!surface) {
      ALLEGRO_ERROR("SDL_GetWindowSurface failed: %s\n", SDL_GetError());
      SDL_DestroyWindow(soft->sdl.window);
      al_free(soft);
      return NULL;
   }

   /* Match the surface format so flipping is a plain row copy. */
   format = _al_sdl_get_allegro_pixel_format(surface->format->format);
   if (format == ALLEGRO_PIXEL_FORMAT_ANY)
      format = ALLEGRO_PIXEL_FORMAT_ABGR_8888;
   soft->sdl_format = _al_sdl_get_sdl_pixel_format(format);
   d->backbuffer_format = format;

   soft->backbuffer = _al_create_bitmap_params(NULL, w, h, format,
      ALLEGRO_MEMORY_BITMAP, 0, 0);
   if (!soft->backbuffer) {
      ALLEGRO_ERROR("Failed to create the %dx%d backbuffer\n", w, h);
      SDL_DestroyWindow(soft->sdl.window);
      al_free(soft);
      return NULL;
   }
   soft->backbuffer->_display = d;

   ALLEGRO_INFO("Software display %dx%d, format %d\n", w, h, format);

   add = _al_vector_alloc_back(&al_get_system_driver()->displays);
   *add = d;

   _al_event_source_init(&d->es);
   d->vt = vt;
   d->extra_settings.settings[ALLEGRO_COMPATIBLE_DISPLAY] = true;

   return d;
}

static ALLEGRO_DISPLAY *soft_create_display(int w, int h)
{
   ALLEGRO_SYSTEM_SDL *s = (void *)al_get_system_driver();
   ALLEGRO_DISPLAY *d;
   al_lock_mutex(s->mutex);
   d = soft_create_display_locked(w, h);
   al_unlock_mutex(s->mutex);
   return d;
}

static void soft_destroy_display(ALLEGRO_DISPLAY *d)
{
   ALLEGRO_SYSTEM_SDL *s = (void *)al_get_system_driver();
   ALLEGRO_DISPLAY_SDL_SOFT *soft = (void *)d;

   al_lock_mutex(s->mutex);
   _al_vector_find_and_delete(&s->system.displays, &d);
   al_unlock_mutex(s->mutex);

   /* Never holds anything: there are no video bitmaps. */
   _al_vector_free(&d->bitmaps);
   _al_vector_free(&d->display_invalidated_callbacks);
   _al_vector_free(&d->display_validated_callbacks);
   _al_event_source_free(&d->es);
   al_free(d->vertex_cache);

   /* Not registered as a destructor or for conversion, so free it directly
    * rather than through al_destroy_bitmap().
    */
   al_free(soft->backbuffer->memory);
   al_free(soft->backbuffer);

   SDL_DestroyWindow(soft->sdl.window);
   al_free(soft);
}

static void soft_update_display_region(ALLEGRO_DISPLAY *d, int x, int y,
   int width, int height)
{
   ALLEGRO_DISPLAY_SDL_SOFT *soft = (void *)d;
   ALLEGRO_BITMAP *bb = soft->backbuffer;
   SDL_Surface *surface = SDL_GetWindowSurface(soft->sdl.window);
   SDL_Rect rect;
   const char *src;
   char *dst;

   if (!surface)
      return;

   if (x < 0) {
      width += x;
      x = 0;
   }
   if (y < 0) {
      height += y;
      y = 0;
   }
   width = _ALLEGRO_MIN(width, _ALLEGRO_MIN(bb->w, surface->w) - x);
   height = _ALLEGRO_MIN(height, _ALLEGRO_MIN(bb->h, surface->h) - y);
   if (width <= 0 || height <= 0)
      return;

   if (SDL_MUSTLOCK(surface) && SDL_LockSurface(surface) < 0)
      return;
   src = (const char *)bb->memory + y * bb->pitch +
      x * al_get_pixel_size(bb->_format);
   dst = (char *)surface->pixels + y * surface->pitch +
      x * surface->format->BytesPerPixel;
   /* Same format: SDL does a memcpy per row. */
   SDL_ConvertPixels(width, height, soft->sdl_format, src, bb->pitch,
      surface->format->format, dst, surface->pitch);
   if (SDL_MUSTLOCK(surface))
      SDL_UnlockSurface(surface);

   rect.x = x;
   rect.y = y;
   rect.w = width;
   rect.h = height;
   SDL_UpdateWindowSurfaceRects(soft->sdl.window, &rect, 1);
}

static void soft_flip_display(ALLEGRO_DISPLAY *d)
{
   soft_update_display_region(d, 0, 0, d->w, d->h);
}

static ALLEGRO_BITMAP *soft_get_backbuffer(ALLEGRO_DISPLAY *d)
{
   return ((ALLEGRO_DISPLAY_SDL_SOFT *)d)->backbuffer;
}

static bool soft_is_compatible_bitmap(ALLEGRO_DISPLAY *display,
   ALLEGRO_BITMAP *bitmap)
{
   (void)display;
   (void)bitmap;
   return true;
}

static void soft_noop(ALLEGRO_DISPLAY *d)
{
   (void)d;
}

static void soft_update_transformation(ALLEGRO_DISPLAY *d,
   ALLEGRO_BITMAP *target)
{
   (void)d;
   (void)target;
}

static bool soft_set_mouse_cursor(ALLEGRO_DISPLAY *display,
   ALLEGRO_MOUSE_CURSOR *cursor)
{
   (void)display;
   (void)cursor;
   return false;
}

static bool soft_set_system_mouse_cursor(ALLEGRO_DISPLAY *display,
   ALLEGRO_SYSTEM_MOUSE_CURSOR cursor_id)
{
   (void)display;
   (void)cursor_id;
   return false;
}

static bool soft_show_hide_mouse_cursor(ALLEGRO_DISPLAY *display)
{
   /* There is no pointer to show or hide. */
   (void)display;
   return true;
}

ALLEGRO_DISPLAY_INTERFACE *_al_sdl_display_driver(void)
{
   if (vt)
      return vt;

   /* Members left NULL are either NULL-checked by Allegro, or only called
    * for video bitmaps / OpenGL, which this display never has.
    */
   vt = al_calloc(1, sizeof *vt);
   vt->id = AL_ID('S', 'D', 'L', 'S');
   vt->create_display = soft_create_display;
   vt->destroy_display = soft_destroy_display;
   vt->flip_display = soft_flip_display;
   vt->update_display_region = soft_update_display_region;
   vt->get_backbuffer = soft_get_backbuffer;
   vt->is_compatible_bitmap = soft_is_compatible_bitmap;
   vt->flush_vertex_cache = soft_noop;
   vt->update_transformation = soft_update_transformation;
   vt->set_mouse_cursor = soft_set_mouse_cursor;
   vt->set_system_mouse_cursor = soft_set_system_mouse_cursor;
   vt->show_mouse_cursor = soft_show_hide_mouse_cursor;
   vt->hide_mouse_cursor = soft_show_hide_mouse_cursor;

   return vt;
}

/* vim: set sts=3 sw=3 et: */
