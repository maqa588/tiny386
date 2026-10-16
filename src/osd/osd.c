#include "osd.h"
#include "microui.h"
#include "atlas.inl"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../vga.h" // XXX: for BPP definition
#include "../misc.h"
#include "../ide.h"
#include "../pc.h"

#define OSD_MAX_FILES 128

typedef struct {
	char name[64];
	int is_dir;
	int64_t size;
} OSDFileEntry;

enum {
	BROWSE_TARGET_NONE = 0,
	BROWSE_TARGET_FDA  = 1,
	BROWSE_TARGET_CDB  = 2,
	BROWSE_TARGET_CDC  = 3,	/* virtual CD-ROM from a host shared folder */
};

struct OSD {
	mu_Context ctx;
	PC *pc;
	EMULINK *emulink;
	IDEIFState *ide, *ide2;
	void *console;

	/* File browser state */
	int browse_target;
	char current_dir[256];
	OSDFileEntry files[OSD_MAX_FILES];
	int file_count;
};

static void format_size(char *buf, size_t bufsz, int64_t bytes)
{
	if (bytes <= 0) {
		snprintf(buf, bufsz, "0 B");
	} else if (bytes < 1024) {
		snprintf(buf, bufsz, "%lld B", (long long)bytes);
	} else if (bytes < 1024 * 1024) {
		snprintf(buf, bufsz, "%.1f KB", (double)bytes / 1024.0);
	} else if (bytes < 1024LL * 1024 * 1024) {
		snprintf(buf, bufsz, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
	} else {
		snprintf(buf, bufsz, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
	}
}

static const char *get_basename(const char *path)
{
	if (!path || !path[0]) return "";
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static int is_image_file(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot) return 0;
	if (strcasecmp(dot, ".img") == 0 ||
	    strcasecmp(dot, ".iso") == 0 ||
	    strcasecmp(dot, ".bin") == 0 ||
	    strcasecmp(dot, ".ima") == 0 ||
	    strcasecmp(dot, ".flp") == 0 ||
	    strcasecmp(dot, ".vhd") == 0)
		return 1;
	return 0;
}

static int compare_entries(const void *a, const void *b)
{
	const OSDFileEntry *ea = a;
	const OSDFileEntry *eb = b;
	if (ea->is_dir != eb->is_dir)
		return eb->is_dir - ea->is_dir; /* directories first */
	return strcasecmp(ea->name, eb->name);
}

static void osd_scan_dir(OSD *osd)
{
	osd->file_count = 0;

	DIR *d = opendir(osd->current_dir[0] ? osd->current_dir : ".");
	if (!d) {
#if defined(BUILD_ESP32)
		strncpy(osd->current_dir, "/sdcard", sizeof(osd->current_dir) - 1);
#else
		strncpy(osd->current_dir, ".", sizeof(osd->current_dir) - 1);
#endif
		d = opendir(osd->current_dir);
		if (!d) return;
	}

	/* Add ".." if not at root */
	if (strcmp(osd->current_dir, "/") != 0 &&
#if defined(BUILD_ESP32)
	    strcmp(osd->current_dir, "/sdcard") != 0 &&
#endif
	    strcmp(osd->current_dir, ".") != 0) {
		strncpy(osd->files[osd->file_count].name, "..", sizeof(osd->files[0].name) - 1);
		osd->files[osd->file_count].is_dir = 1;
		osd->files[osd->file_count].size = 0;
		osd->file_count++;
	}

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL && osd->file_count < OSD_MAX_FILES) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;
		if (ent->d_name[0] == '.')
			continue;

		char fullpath[512];
		snprintf(fullpath, sizeof(fullpath), "%s/%s", osd->current_dir, ent->d_name);

		struct stat st;
		int is_dir = 0;
		int64_t size = 0;
		if (stat(fullpath, &st) == 0) {
			if (S_ISDIR(st.st_mode))
				is_dir = 1;
			else
				size = st.st_size;
		}

		if (is_dir || is_image_file(ent->d_name)) {
			strncpy(osd->files[osd->file_count].name, ent->d_name, sizeof(osd->files[0].name) - 1);
			osd->files[osd->file_count].is_dir = is_dir;
			osd->files[osd->file_count].size = size;
			osd->file_count++;
		}
	}
	closedir(d);

	if (osd->file_count > 1) {
		int start = (strcmp(osd->files[0].name, "..") == 0) ? 1 : 0;
		qsort(&osd->files[start], osd->file_count - start, sizeof(OSDFileEntry), compare_entries);
	}
}

static void osd_cd_up(OSD *osd)
{
	char *slash = strrchr(osd->current_dir, '/');
	if (slash) {
		if (slash == osd->current_dir)
			osd->current_dir[1] = '\0';
		else
			*slash = '\0';
	} else {
		strcpy(osd->current_dir, ".");
	}
	osd_scan_dir(osd);
}

static void osd_cd_into(OSD *osd, const char *name)
{
	char newpath[256];
	if (strcmp(osd->current_dir, ".") == 0)
		snprintf(newpath, sizeof(newpath), "%s", name);
	else if (strcmp(osd->current_dir, "/") == 0)
		snprintf(newpath, sizeof(newpath), "/%s", name);
	else
		snprintf(newpath, sizeof(newpath), "%s/%s", osd->current_dir, name);
	strncpy(osd->current_dir, newpath, sizeof(osd->current_dir) - 1);
	osd_scan_dir(osd);
}

static void osd_select_file(OSD *osd, const char *name)
{
	char fullpath[256];
	if (strcmp(osd->current_dir, ".") == 0)
		snprintf(fullpath, sizeof(fullpath), "%s", name);
	else if (strcmp(osd->current_dir, "/") == 0)
		snprintf(fullpath, sizeof(fullpath), "/%s", name);
	else
		snprintf(fullpath, sizeof(fullpath), "%s/%s", osd->current_dir, name);

	struct stat st;
	int64_t size = 0;
	if (stat(fullpath, &st) == 0)
		size = st.st_size;

	if (osd->browse_target == BROWSE_TARGET_FDA) {
		if (osd->emulink)
			emulink_attach_floppy(osd->emulink, 0, fullpath);
		if (osd->pc) {
			strncpy(osd->pc->fda_path, fullpath, sizeof(osd->pc->fda_path) - 1);
			osd->pc->fda_size = size;
		}
	} else if (osd->browse_target == BROWSE_TARGET_CDB) {
		if (osd->ide)
			ide_change_cd(osd->ide, 1, fullpath);
		if (osd->pc) {
			strncpy(osd->pc->cdb_path, fullpath, sizeof(osd->pc->cdb_path) - 1);
			osd->pc->cdb_size = size;
		}
	}
	osd->browse_target = BROWSE_TARGET_NONE;
}

/* Mount the browsed directory as the virtual CD-ROM (cdc).  It shares the
   primary slave slot with the ISO-image drive (cdb), since Win9x drives only
   the primary IDE channel: mounting the folder ejects any ISO there.  The
   currently mounted images are registered as forbidden so the synthesised
   disc can never include the system image, the floppy image or the CD ISO. */
static void osd_mount_folder(OSD *osd)
{
	if (osd->pc)
		ide_set_vcd_forbidden(osd->pc->hda_path, osd->pc->fda_path,
				      osd->pc->cdb_path);
	if (osd->ide)
		ide_change_cd(osd->ide, 1, osd->current_dir);
	/* pc->cdc_path is updated by the pc CD-change callback on success */
	osd->browse_target = BROWSE_TARGET_NONE;
}

static void do_window(mu_Context *ctx, struct OSD *osd)
{
	if (osd->browse_target != BROWSE_TARGET_NONE) {
		/* File Browser View */
		const char *title = (osd->browse_target == BROWSE_TARGET_FDA) ?
			"Select Floppy (fda)" :
			(osd->browse_target == BROWSE_TARGET_CDC) ?
			"Select Shared Folder (cdc)" : "Select CD-ROM (cdb)";
		if (mu_begin_window_ex(ctx, title, mu_rect(20, 20, 360, 310), MU_OPT_NOCLOSE)) {
			char path_lbl[300];
			snprintf(path_lbl, sizeof(path_lbl), "Path: %s", osd->current_dir);
			mu_layout_row(ctx, 1, (int[]) { -1 }, 0);
			mu_label(ctx, path_lbl);

			mu_layout_row(ctx, 3, (int[]) { 75, 75, 75 }, 0);
			if (mu_button(ctx, "Up (..)"))
				osd_cd_up(osd);
			if (mu_button(ctx, "Refresh"))
				osd_scan_dir(osd);
			if (mu_button(ctx, "Cancel"))
				osd->browse_target = BROWSE_TARGET_NONE;

			if (osd->browse_target == BROWSE_TARGET_CDC) {
				char mnt_lbl[300];
				snprintf(mnt_lbl, sizeof(mnt_lbl), "Mount This Folder: %s",
					 osd->current_dir);
				mu_layout_row(ctx, 1, (int[]) { -1 }, 0);
				if (mu_button(ctx, mnt_lbl))
					osd_mount_folder(osd);
			}

			mu_layout_row(ctx, 1, (int[]) { -1 }, 200);
			mu_begin_panel(ctx, "file_panel");
			for (int i = 0; i < osd->file_count; i++) {
				/* the shared-folder CD is directory-based: browse dirs only */
				if (osd->browse_target == BROWSE_TARGET_CDC &&
				    !osd->files[i].is_dir)
					continue;
				mu_layout_row(ctx, 1, (int[]) { -1 }, 0);
				if (osd->files[i].is_dir) {
					char label[100];
					snprintf(label, sizeof(label), "[DIR] %s", osd->files[i].name);
					if (mu_button(ctx, label)) {
						if (strcmp(osd->files[i].name, "..") == 0)
							osd_cd_up(osd);
						else
							osd_cd_into(osd, osd->files[i].name);
					}
				} else {
					char sz_buf[32];
					format_size(sz_buf, sizeof(sz_buf), osd->files[i].size);
					char label[120];
					snprintf(label, sizeof(label), "%s (%s)", osd->files[i].name, sz_buf);
					if (mu_button(ctx, label)) {
						osd_select_file(osd, osd->files[i].name);
					}
				}
			}
			mu_end_panel(ctx);
			mu_end_window(ctx);
		}
		return;
	}

	/* Main Control Panel View */
	if (mu_begin_window_ex(ctx, "Tiny386 Control Panel",
			       mu_rect(30, 20, 360, 400),
			       MU_OPT_NOCLOSE)) {

		mu_layout_row(ctx, 1, (int[]) { -1 }, 0);
		mu_label(ctx, "--- Storage Drives ---");

		/* HDA: Primary Master HDD (Read-only / Locked) */
		char hda_info[200];
		if (osd->pc && osd->pc->hda_path[0]) {
			char sz_buf[32];
			format_size(sz_buf, sizeof(sz_buf), osd->pc->hda_size);
			snprintf(hda_info, sizeof(hda_info), "hda: %s (%s) [Locked]",
				 get_basename(osd->pc->hda_path), sz_buf);
		} else {
			snprintf(hda_info, sizeof(hda_info), "hda: [None]");
		}
		mu_layout_row(ctx, 1, (int[]) { -1 }, 0);
		mu_label(ctx, hda_info);

		/* FDA: Floppy Drive A */
		char fda_info[200];
		if (osd->pc && osd->pc->fda_path[0]) {
			char sz_buf[32];
			format_size(sz_buf, sizeof(sz_buf), osd->pc->fda_size);
			snprintf(fda_info, sizeof(fda_info), "fda: %s (%s)",
				 get_basename(osd->pc->fda_path), sz_buf);
		} else {
			snprintf(fda_info, sizeof(fda_info), "fda: [Empty / No Floppy]");
		}
		mu_layout_row(ctx, 1, (int[]) { -1 }, 0);
		mu_label(ctx, fda_info);

		mu_layout_row(ctx, 2, (int[]) { 110, 80 }, 0);
		if (mu_button(ctx, "Mount Floppy")) {
			osd->browse_target = BROWSE_TARGET_FDA;
			osd_scan_dir(osd);
		}
		if (mu_button(ctx, "Eject fda")) {
			if (osd->emulink)
				emulink_attach_floppy(osd->emulink, 0, NULL);
			if (osd->pc) {
				osd->pc->fda_path[0] = '\0';
				osd->pc->fda_size = 0;
			}
		}

		/* CDB: CD-ROM Drive (primary slave, ISO image) */
		char cdb_info[200];
		if (osd->pc && osd->pc->cdb_path[0]) {
			char sz_buf[32];
			format_size(sz_buf, sizeof(sz_buf), osd->pc->cdb_size);
			snprintf(cdb_info, sizeof(cdb_info), "cdb: %s (%s)",
				 get_basename(osd->pc->cdb_path), sz_buf);
		} else {
			snprintf(cdb_info, sizeof(cdb_info), "cdb: [Empty / No Disc]");
		}
		mu_layout_row(ctx, 1, (int[]) { -1 }, 0);
		mu_label(ctx, cdb_info);

		mu_layout_row(ctx, 2, (int[]) { 110, 80 }, 0);
		if (mu_button(ctx, "Mount CD/ISO")) {
			osd->browse_target = BROWSE_TARGET_CDB;
			osd_scan_dir(osd);
		}
		if (mu_button(ctx, "Eject cdb")) {
			if (osd->ide)
				ide_change_cd(osd->ide, 1, "");
			if (osd->pc) {
				osd->pc->cdb_path[0] = '\0';
				osd->pc->cdb_size = 0;
			}
		}

		/* CDC: virtual CD-ROM from a host shared folder (primary slave,
		   mutually exclusive with cdb) */
		char cdc_info[220];
		if (osd->pc && osd->pc->cdc_path[0]) {
			snprintf(cdc_info, sizeof(cdc_info), "cdc: %s [Shared Folder]",
				 get_basename(osd->pc->cdc_path));
		} else {
			snprintf(cdc_info, sizeof(cdc_info), "cdc: [Empty / No Disc]");
		}
		mu_layout_row(ctx, 1, (int[]) { -1 }, 0);
		mu_label(ctx, cdc_info);

		mu_layout_row(ctx, 2, (int[]) { 130, 90 }, 0);
		if (mu_button(ctx, "Mount Folder...")) {
			osd->browse_target = BROWSE_TARGET_CDC;
			osd_scan_dir(osd);
		}
		if (mu_button(ctx, "Eject cdc")) {
			if (osd->ide)
				ide_change_cd(osd->ide, 1, "");
			if (osd->pc) {
				osd->pc->cdc_path[0] = '\0';
				osd->pc->cdc_size = 0;
			}
		}

		/* System Controls */
		mu_layout_row(ctx, 1, (int[]) { -1 }, 0);
		mu_label(ctx, "--- System Controls ---");

		if (osd->console) {
			void __attribute__((weak)) console_send_kbd(void *opaque, int keypress, int keycode);
			void __attribute__((weak)) console_power_off(void *opaque);
			void __attribute__((weak)) console_trigger_suspend(void *opaque);
			if (console_send_kbd) {
				mu_layout_row(ctx, 3, (int[]) { 75, 75, 75 }, 0);
				if (mu_button(ctx, "Ctrl+Alt+Del")) {
					console_send_kbd(osd->console, 1, 0x1d);
					console_send_kbd(osd->console, 1, 0x38);
					console_send_kbd(osd->console, 1, 0x53);
					console_send_kbd(osd->console, 0, 0x53);
					console_send_kbd(osd->console, 0, 0x38);
					console_send_kbd(osd->console, 0, 0x1d);
				}
				if (mu_button(ctx, "Ctrl+[")) {
					console_send_kbd(osd->console, 1, 0x1d);
					console_send_kbd(osd->console, 1, 0x1a);
					console_send_kbd(osd->console, 0, 0x1a);
					console_send_kbd(osd->console, 0, 0x1d);
				}
				if (mu_button(ctx, "Ctrl+]")) {
					console_send_kbd(osd->console, 1, 0x1d);
					console_send_kbd(osd->console, 1, 0x1b);
					console_send_kbd(osd->console, 0, 0x1b);
					console_send_kbd(osd->console, 0, 0x1d);
				}
			}
			mu_layout_row(ctx, 2, (int[]) { 116, 116 }, 0);
			if (console_trigger_suspend && mu_button(ctx, "Sleep / Suspend")) {
				console_trigger_suspend(osd->console);
			}
			if (console_power_off && mu_button(ctx, "Power Off")) {
				console_power_off(osd->console);
			}
		}

		mu_end_window(ctx);
	}
}


static void process_frame(mu_Context *ctx, struct OSD *osd)
{
	mu_begin(ctx);
	do_window(ctx, osd);
	mu_end(ctx);
}

// raw render
static int text_width(mu_Font font, const char *text, int len)
{
	int res = 0;
	for (const char *p = text; *p && len--; p++) {
		if ((*p & 0xc0) == 0x80) { continue; }
		int chr = mu_min((unsigned char) *p, 127);
		res += atlas[ATLAS_FONT + chr].w;
	}
	return res;
}

static int text_height(mu_Font font)
{
	return 18;
}

static const char kb_ascii[2][128] =
{
	{
		0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
		'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0,
		'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
		'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, 0, 0, ' ',
	},
	{
		0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
		'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, 0,
		'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
		'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, 0, 0, ' ',
	},
};

static char scancode_trans(int code, int i)
{
	return kb_ascii[i][code & 0x7f];
}

static void render(mu_Context *ctx,
		   uint8_t *pixels,
		   int w, int h, int pitch)
{
	mu_Command *cmd = NULL;
	int clipx = 0;
	int clipy = 0;
	int clipw = 0x1000000;
	int cliph = 0x1000000;
	while(mu_next_command(ctx, &cmd)) {
		switch (cmd->type) {
		case MU_COMMAND_TEXT: {
			int destx = cmd->text.pos.x;
			int desty = cmd->text.pos.y;

			for(const char *p = cmd->text.str; *p; p++) {
				if((*p & 0xc0) == 0x80) { continue; }
				int chr = mu_min((unsigned char) *p, 127);
				mu_Rect src = atlas[ATLAS_FONT + chr];
				int srcx = src.x;
				int srcy = src.y;
				int srcw = src.w;
				int srch = src.h;

				for (int y = 0; y < srch; y++) {
					if (y + desty < clipy || y + desty >= clipy + cliph || y + desty >= h)
						continue;
					for (int x = 0; x < srcw; x++) {
						if (x + destx < clipx || x + destx >= clipx + clipw || x + destx >= w)
							continue;
						uint8_t c = atlas_texture[(y + srcy) * 128 + (x + srcx)];
						if (c) {
#if BPP == 32
							uint32_t cc = c | (c << 8) | (c << 16) | (255 << 24);
							*(uint32_t *)&(pixels[(y + desty) * pitch + 4 * (x + destx)]) = cc;
#elif BPP == 16
							uint16_t cc = (c >> 3) | ((c >> 2) << 5) | ((c >> 3) << 11);
							*(uint16_t *)&(pixels[(y + desty) * pitch + 2 * (x + destx)]) = cc;
#else
#error "bad bpp"
#endif
						}
					}
				}
				destx += srcw;
			}
			break;
		}
		case MU_COMMAND_RECT: {
			int rectx = cmd->rect.rect.x;
			int recty = cmd->rect.rect.y;
			int rectw = cmd->rect.rect.w;
			int recth = cmd->rect.rect.h;
#if BPP == 32
			uint32_t cc = cmd->rect.color.b | (cmd->rect.color.g << 8) |
				(cmd->rect.color.r << 16) | (cmd->rect.color.a << 24);
#elif BPP == 16
			uint16_t cc = (cmd->rect.color.b >> 3) | ((cmd->rect.color.g >> 2) << 5) |
				((cmd->rect.color.r >> 3) << 11);
#else
#error "bad bpp"
#endif
			for (int y = recty; y < recty + recth; y++) {
				if (y < clipy || y >= clipy + cliph || y >= h)
					continue;
				for (int x = rectx; x < rectx + rectw; x++) {
					if (x < clipx || x >= clipx + clipw || x >= w)
						continue;
#if BPP == 32
					*(uint32_t *)&(pixels[y * pitch + 4 * x]) = cc;
#elif BPP == 16
					*(uint16_t *)&(pixels[y * pitch + 2 * x]) = cc;
#else
#error "bad bpp"
#endif
				}
			}
			break;
		}
		case MU_COMMAND_ICON: {
			int rectx = cmd->icon.rect.x;
			int recty = cmd->icon.rect.y;
			int rectw = cmd->icon.rect.w;
			int recth = cmd->icon.rect.h;
			mu_Rect r = atlas[cmd->icon.id];
			recty += (recth - r.h) / 2;
			rectx += (rectw - r.w) / 2;
			for (int y = 0; y < r.h; y++) {
				if (y + recty < clipy || y + recty >= clipy + cliph || y + recty >= h)
					continue;
				for (int x = 0; x < r.w; x++) {
					if (x + rectx < clipx || x + rectx >= clipx + clipw || x + rectx >= w)
						continue;
					uint8_t c = atlas_texture[(y + r.y) * 128 + (x + r.x)];
#if BPP == 32
					uint32_t cc = c | (c << 8) | (c << 16) | (255 << 24);
					*(uint32_t *)&(pixels[(y + recty) * pitch + 4 * (x + rectx)]) = cc;
#elif BPP == 16
					uint16_t cc = (c >> 3) | ((c >> 2) << 5) | ((c >> 3) << 11);
					*(uint16_t *)&(pixels[(y + recty) * pitch + 2 * (x + rectx)]) = cc;
#else
#error "bad bpp"
#endif

				}
			}
			break;
		}
		case MU_COMMAND_CLIP: {
			clipx = cmd->clip.rect.x;
			clipy = cmd->clip.rect.y;
			clipw = cmd->clip.rect.w;
			cliph = cmd->clip.rect.h;
			break;
		}
		}
	}

	// draw cursor
	if (ctx->mouse_pos.x || ctx->mouse_pos.y || ctx->last_mouse_pos.x || ctx->last_mouse_pos.y) {
#if BPP == 32
		uint32_t cc = 255 | (255 << 8) |
			(255 << 16) | (255 << 24);
#elif BPP == 16
		uint16_t cc = (255 >> 3) | ((255 >> 2) << 5) |
			((255 >> 3) << 11);
#else
#error "bad bpp"
#endif
		for (int y = ctx->mouse_pos.y; y < ctx->mouse_pos.y + 8; y++) {
			if (y < 0 || y + 8 >= h)
				continue;
			for (int x = ctx->mouse_pos.x; x < ctx->mouse_pos.x + 8; x++) {
				if (x < 0 || x + 8 >= w)
					continue;
				if (x - ctx->mouse_pos.x > 5 - (y - ctx->mouse_pos.y) &&
				    x - ctx->mouse_pos.x != y - ctx->mouse_pos.y)
					continue;
#if BPP == 32
				*(uint32_t *)&(pixels[y * pitch + 4 * x]) = cc;
#elif BPP == 16
				*(uint16_t *)&(pixels[y * pitch + 2 * x]) = cc;
#else
#error "bad bpp"
#endif
			}
		}
	}
}

OSD *osd_init()
{
	OSD *osd = malloc(sizeof(OSD));
	memset(osd, 0, sizeof(OSD));
#if defined(BUILD_ESP32)
	strncpy(osd->current_dir, "/sdcard", sizeof(osd->current_dir) - 1);
#else
	strncpy(osd->current_dir, ".", sizeof(osd->current_dir) - 1);
#endif
	mu_init(&(osd->ctx));
	osd->ctx.text_height = text_height;
	osd->ctx.text_width = text_width;
	return osd;
}

void osd_attach_pc(OSD *osd, void *pc)
{
	osd->pc = pc;
}

void osd_attach_emulink(OSD *osd, void *emulink)
{
	osd->emulink = emulink;
}

void osd_attach_ide(OSD *osd, void *ide, void *ide2)
{
	osd->ide = ide;
	osd->ide2 = ide2;
}

void osd_attach_console(OSD *osd, void *console)
{
	osd->console = console;
}


void osd_handle_mouse_motion(OSD *osd, int x, int y)
{
	mu_input_mousemove(&(osd->ctx), x, y);
}

void osd_handle_mouse_button(OSD *osd, int x, int y, int down, int btn)
{
	int b = 0;
	switch (btn) {
	case 1: b = MU_MOUSE_LEFT; break;
	case 2: b = MU_MOUSE_RIGHT; break;
	case 4: b = MU_MOUSE_MIDDLE; break;
	}
	if (b) {
		if (down)
			mu_input_mousedown(&(osd->ctx), x, y, b);
		else
			mu_input_mouseup(&(osd->ctx), x, y, b);
	}
}

void osd_handle_key(OSD *osd, int keycode, int down)
{
	int c = 0;
	switch (keycode) {
	case 0x1c: c = MU_KEY_RETURN; break;
	case 0x0e: c = MU_KEY_BACKSPACE; break;
	case 0x2a: case 0x36: c = MU_KEY_SHIFT; break;
	case 0x1d: case 0x61: c = MU_KEY_CTRL; break;
	case 0x38: case 0x64: c = MU_KEY_ALT; break;
	}
	if (c) {
		if (down)
			mu_input_keydown(&(osd->ctx), c);
		else
			mu_input_keyup(&(osd->ctx), c);
	}
	if (down) {
		char buf[2] = {0, 0};
		buf[0] = scancode_trans(keycode, osd->ctx.key_down & MU_KEY_SHIFT);
		if (buf[0])
			mu_input_text(&(osd->ctx), buf);
	}
}

void osd_render(OSD *osd, uint8_t *pixels, int w, int h, int pitch)
{
	process_frame(&(osd->ctx), osd);
	render(&(osd->ctx), pixels, w, h, pitch);
}
