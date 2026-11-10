// SPDX-License-Identifier: GPL-2.0
/*
 * Generic MIPI-DSI panel driver
 * Copyright (C) 2024 ROCKNIX
 *
 * based on
 *
 * Rockteck jh057n00900 5.5" MIPI-DSI panel driver
 * Copyright (C) Purism SPC 2019
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/hex.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/firmware.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/version.h>

#include <video/display_timing.h>
#include <video/mipi_display.h>

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(7, 3, 0))
#include <drm/drm_of.h>
#endif
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_client.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#define DRIVER_NAME "panel-generic-dsi"

static char *descfile = "";
module_param(descfile,charp,0660);
MODULE_PARM_DESC(descfile, "Panel description filename in firmware dir");

struct generic_panel_delays {
    int prepare;
    int reset;
    int init;
    int enable;
    int ready;
};

struct generic_panel_size {
    int width;
    int height;
};

struct generic_panel_mode {
    int clock;
    int horizontal[5];
    int vertical[5];
    int is_default;
    struct generic_panel_mode *prev;
};

#define DCS_PSEUDO_CMD_SEQ 0x10000
struct generic_panel_init_seq {
    int dcs;
    int len;
    int read;
    int wait;
    u8 *data;
    struct generic_panel_init_seq *link;
};

struct generic_panel {
    struct device *dev;
    struct drm_panel panel;
    struct drm_connector *connector;
    int cur_timing;
    struct gpio_desc *enable_gpio;
    struct gpio_desc *reset_gpio;
    struct regulator *vdd;
    struct regulator *iovcc;

    struct generic_panel_delays delays;
    struct generic_panel_size size;
    struct generic_panel_mode *modes;
    struct generic_panel_init_seq *iseq;

    enum drm_panel_orientation orientation;
    bool prepared;
};


int load_panel_description(struct mipi_dsi_device *dsi, struct generic_panel *ctx);
int load_panel_description_line(char *data, struct mipi_dsi_device *dsi, struct generic_panel *ctx);
int load_globals(char *data, struct mipi_dsi_device *dsi, struct generic_panel *ctx);
int load_mode(char *data, struct mipi_dsi_device *dsi, struct generic_panel *ctx);
int load_init_seq(char *data, struct mipi_dsi_device *dsi, struct generic_panel *ctx);



int load_globals(char *data, struct mipi_dsi_device *dsi, struct generic_panel *ctx) {
    struct device *dev = &dsi->dev;
    char *param, *val;
    while (*data) {
        data = next_arg(data, &param, &val);
        if (!val) continue;
        if (strcmp(param, "delays") == 0) {
            int delays[] = {0, 5, 1, 25, 120, 50};
            get_options(val, 6, delays);
            ctx->delays.prepare     = delays[1];
            ctx->delays.reset       = delays[2];
            ctx->delays.init        = delays[3];
            ctx->delays.enable      = delays[4];
            ctx->delays.ready       = delays[5];
        } else if (strcmp(param, "size") == 0) {
            int size[] = {0, -1, -1};
            get_options(val, 3, size);
            ctx->size.width         = size[1];
            ctx->size.height        = size[2];
        } else if (strcmp(param, "format") == 0) {
            if (strcmp(val, "rgb888") == 0) {
                dsi->format = MIPI_DSI_FMT_RGB888;
            } else if (strcmp(val, "rgb666") == 0) {
                dsi->format = MIPI_DSI_FMT_RGB666;
            } else if (strcmp(val, "rgb666_packed") == 0) {
                dsi->format = MIPI_DSI_FMT_RGB666_PACKED;
            } else if (strcmp(val, "rgb565") == 0) {
                dsi->format = MIPI_DSI_FMT_RGB565;
            } else {
                dev_info(dev, "bad format %s\n", val);
            }
        } else if (strcmp(param, "lanes") == 0) {
            if (get_option(&val, &dsi->lanes) == 0) {
                dev_info(dev, "bad lanes %s\n", val);
            }
        } else if (strcmp(param, "flags") == 0) {
            int flags;
            if (get_option(&val, &flags) == 0) {
                dev_info(dev, "bad flags %s\n", val);
            } else {
                dsi->mode_flags = flags;
            }
        } else {
            dev_info(dev, "unknown param %s\n", param);
            return -1;
        }
    }
    return 0;
}


int load_mode(char *data, struct mipi_dsi_device *dsi, struct generic_panel *ctx) {
    struct device *dev = &dsi->dev;
    struct generic_panel_mode *mode;
    char *param, *val;

    mode = devm_kzalloc(dev, sizeof(*mode), GFP_KERNEL);

    while (*data) {
        data = next_arg(data, &param, &val);
        if (!val) continue;
        if (strcmp(param, "clock") == 0) {
            if (get_option(&val, &mode->clock) == 0) {
                dev_info(dev, "bad clock %s\n", val);
            }
        } else if (strcmp(param, "horizontal") == 0) {
            get_options(val, 5, &mode->horizontal[0]);
        } else if (strcmp(param, "vertical") == 0) {
            get_options(val, 5, &mode->vertical[0]);
        } else if (strcmp(param, "default") == 0) {
            get_option(&val, &mode->is_default);
        } else {
            dev_info(dev, "Mode unhandled %s = %s\n", param, val);
        }
    }

    if ((mode->clock > 5000) && (mode->vertical[1] > 0) && (mode->horizontal[1] > 0)) {
        mode->prev = ctx->modes;
        ctx->modes = mode;
        dev_dbg(dev, "Mode %d %d %d  %p -> %p\n", mode->clock, mode->vertical[1], mode->horizontal[1], mode, mode->prev);
        return 0;
    }
    return -1;
}

int load_init_seq(char *data, struct mipi_dsi_device *dsi, struct generic_panel *ctx) {
    struct device *dev = &dsi->dev;
    struct generic_panel_init_seq *item;
    char *param, *val;

    item = devm_kzalloc(dev, sizeof(*item), GFP_KERNEL);
    item->dcs = -1;
    item->len = -1;
    item->read = 0;
    item->wait = 0;

    while (*data) {
        data = next_arg(data, &param, &val);
        if (!val) continue;
        if (strcmp(param, "dcs") == 0) {
            item->dcs = simple_strtoul(val, NULL, 16) & 0xFF;
            //dev_info(dev, "Init dcs %02x\n", item->dcs);
        } else if (strcmp(param, "data") == 0) {
            item->len = (strlen(val)) >> 1;
            item->data = devm_kzalloc(dev, item->len, GFP_KERNEL);
            if (hex2bin(item->data, val, item->len) != 0) {
                dev_info(dev, "bad data %s\n", val);
                return -1;
            }
            //for (int i=0; i < (item->len); i++) { dev_info(dev, " %02x", item->data[i]);};
            //dev_info(dev, "\n");
        } else if (strcmp(param, "seq") == 0) {
            item->dcs = DCS_PSEUDO_CMD_SEQ;
            item->len = (strlen(val)) >> 1;
            item->data = devm_kzalloc(dev, item->len, GFP_KERNEL);
            if (hex2bin(item->data, val, item->len) != 0) {
                dev_info(dev, "bad seq %s\n", val);
                return -1;
            }
            dev_dbg(dev, "loaded seq len=%d %s\n", item->len, val);
        } else if (strcmp(param, "read") == 0) {
            item->read = simple_strtoul(val, NULL, 16);
        } else if (strcmp(param, "wait") == 0) {
            item->wait = simple_strtoul(val, NULL, 16);
        } else {
            dev_info(dev, "Init unhandled %s = %s\n", param, val);
        }
    }

    if (item->dcs >= 0) {
        item->link = ctx->iseq;
        ctx->iseq = item;
        return 0;
    }

    return -1;
}

/*
 * data   is a null-terminated string of panel description
 */
int load_panel_description_line(char *data, struct mipi_dsi_device *dsi, struct generic_panel *ctx) {
    size_t pos;
    for (pos = 0; data[pos] != 0; pos ++) {
        if ((data[pos] == '#') || data[pos] == '\n') {
            data[pos] = 0;
            break;
        }
    }
    if (pos < 2) return 0;

    switch (data[0]) {
        case 'G':
            load_globals(data+1, dsi, ctx);
            break;
        case 'M':
            load_mode(data+1, dsi, ctx);
            break;
        case 'I':
            load_init_seq(data+1, dsi, ctx);
            break;
        default:
            dev_info(NULL, "Unhandled %s\n", data);
    }

    return 0;
}

int panel_description_foreach(struct mipi_dsi_device *dsi, struct generic_panel *ctx,
        int (*handle_line)(char*, struct mipi_dsi_device*, struct generic_panel*));

int panel_description_foreach(struct mipi_dsi_device *dsi, struct generic_panel *ctx,
        int (*handle_line)(char*, struct mipi_dsi_device*, struct generic_panel*))
{
    struct device *dev = &dsi->dev;
    const struct firmware *fw;
    int ret;

    if (strlen(descfile) > 0) {
        ret = request_firmware(&fw, descfile, dev);
        if (ret) {
            dev_err(dev, "No config file found (error=%d)\n", ret);
            return -1;
        }

        size_t pos = 0, size = fw->size;
        char *data = (char *)fw->data;

        while (pos < size) {
            while ((pos < size) && (data[pos] != '\n')) pos++;

            if (pos < size) pos++;
            data[pos - 1] = 0;

            handle_line(data, dsi, ctx);

            data = &data[pos];
            size = size - pos;
            pos = 0;
        }

        release_firmware(fw);
    } else {
        //ret = of_drm_get_panel_orientation(dev->of_node, &ctx->orientation);
        const char *line;
        char * buf;
        size_t linescnt, i;

        linescnt = of_property_count_strings(dev->of_node, "panel_description");
        if (linescnt < 1) {
            dev_err(dev, "failed to read panel_description from device tree %ld\n", linescnt);
            return -1;
        }

        for (i = 0; i < linescnt; i++) {
            ret = of_property_read_string_index(dev->of_node, "panel_description", i, &line);
            if (ret < 0) {
                dev_err(dev, "failed to read panel_description[%ld] from device tree %d\n", i, ret);
                return -1;
            }
            buf = kstrdup(line, GFP_KERNEL);
            if (!buf) {
                dev_err(dev, "kstrdup(of desc line) failed\n");
                return -1;
            }

            dev_dbg(dev, "desc[%ld]: %s", i, line);
            handle_line(buf, dsi, ctx);
            kfree(buf);
        }
    }
    return 0;
}

int load_panel_description(struct mipi_dsi_device *dsi, struct generic_panel *ctx) {
    int ret;

    ret = panel_description_foreach(dsi, ctx, &load_panel_description_line);
    if (ret < 0) { return ret; }

    // Reverse iseq
    struct generic_panel_init_seq *rev = ctx->iseq, *fwd = NULL, *tmp = NULL;
    while (rev) {
        tmp = rev;
        rev = tmp->link;
        tmp->link = fwd;
        fwd = tmp;
    }
    ctx->iseq = fwd;


    return 0;
}


static inline struct generic_panel *panel_to_generic_panel(struct drm_panel *panel)
{
    return container_of(panel, struct generic_panel, panel);
}

static int generic_panel_init_sequence(struct generic_panel *ctx)
{
    struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
    struct device *dev = ctx->dev;
    int ret;

    struct generic_panel_init_seq *iseq = ctx->iseq;
    while (iseq) {
        if (iseq->read > 0) {
            u8 readbuf[8];
            if (iseq->read > 8) { iseq->read = 8; }
            ret = mipi_dsi_generic_read(dsi, iseq->data, iseq->len, &readbuf[0], iseq->read);
            if (ret < 0) {
                dev_err(ctx->dev, "failed to read: %d\n", ret);
            } else {
                for (int i = 0; i < ret; i++) {
                    dev_info(ctx->dev, "read[%d]: %02x\n", i, readbuf[i]);
                }
            }
        } else if (iseq->dcs == DCS_PSEUDO_CMD_SEQ) {
            ret = mipi_dsi_dcs_write_buffer(dsi, iseq->data, iseq->len);
            dev_dbg(dev, "iseq 0x%px len=%d -> %d\n", (void*)iseq, iseq->len, ret);
        } else {
            ret = mipi_dsi_dcs_write(dsi, iseq->dcs, iseq->data, iseq->len);
            dev_dbg(dev, "iseq %02x len=%d -> %d\n", iseq->dcs, iseq->len, ret);
        }
        if (iseq->wait) {
            msleep(iseq->wait);
        }
        iseq = iseq->link;
    }

    dev_dbg(dev, "Panel init sequence done\n");

    return 0;
}

static int generic_panel_unprepare(struct drm_panel *panel)
{
    struct generic_panel *ctx = panel_to_generic_panel(panel);
    struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
    int ret;

    if (!ctx->prepared)
        return 0;

    /* Both DCS writes below are best-effort: a failure must not abort the power-down */
    ret = mipi_dsi_dcs_set_display_off(dsi);
    if (ret < 0)
        dev_err(ctx->dev, "failed to set display off: %d\n", ret);

    ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
    if (ret < 0)
        dev_err(ctx->dev, "failed to enter sleep mode: %d\n", ret);

    if (ctx->enable_gpio) { gpiod_set_value_cansleep(ctx->enable_gpio, 0); }
    gpiod_set_value_cansleep(ctx->reset_gpio, 1);

    regulator_disable(ctx->iovcc);
    regulator_disable(ctx->vdd);

    ctx->prepared = false;

    return 0;
}

static int generic_panel_prepare(struct drm_panel *panel)
{
    struct generic_panel *ctx = panel_to_generic_panel(panel);
    struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
    int ret;

    if (ctx->prepared) {
        return 0;
    }

    /*
     * The backlight is still on from the bootloader's logo: without this the
     * reset and the init sequence show on screen as corruption and grey.
     * drm_panel_enable() turns it back on once the panel is ready.
     */
    backlight_disable(ctx->panel.backlight);

    ret = regulator_enable(ctx->vdd);
    if (ret < 0) {
        dev_err(ctx->dev, "Failed to enable vdd supply: %d\n", ret);
        return ret;
    }

    ret = regulator_enable(ctx->iovcc);
    if (ret < 0) {
        dev_err(ctx->dev, "Failed to enable iovcc supply: %d\n", ret);
        goto disable_vdd;
    }

    if (ctx->enable_gpio) { gpiod_set_value_cansleep(ctx->enable_gpio, 1); }
    msleep(ctx->delays.prepare);

    gpiod_set_value_cansleep(ctx->reset_gpio, 1);
    msleep(ctx->delays.reset);
    gpiod_set_value_cansleep(ctx->reset_gpio, 0);

    msleep(ctx->delays.init);

    ret = generic_panel_init_sequence(ctx);
    if (ret < 0) {
        dev_err(ctx->dev, "Panel init sequence failed: %d\n", ret);
        goto disable_iovcc;
    }

    ret = mipi_dsi_dcs_set_display_on(dsi);
    if (ret < 0) {
        dev_err(ctx->dev, "Failed to set display on: %d\n", ret);
        goto disable_iovcc;
    }

    ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
    if (ret < 0) {
        dev_err(ctx->dev, "Failed to exit sleep mode: %d\n", ret);
        goto disable_iovcc;
    }

    msleep(ctx->delays.enable);

    //msleep(ctx->delays.ready);

    ctx->prepared = true;

    return 0;

disable_iovcc:
    regulator_disable(ctx->iovcc);
disable_vdd:
    regulator_disable(ctx->vdd);
    return ret;
}

/* drm_display_mode template without clock as it is variable */
static const struct drm_display_mode mode_template = { };


static void generic_panel_fill_mode(struct generic_panel *ctx,
                    const struct generic_panel_mode *genmode,
                    struct drm_display_mode *mode)
{
    *mode = mode_template;

    mode->clock          = genmode->clock;

    mode->hdisplay       = genmode->horizontal[1];
    mode->hsync_start    = mode->hdisplay + genmode->horizontal[2];
    mode->hsync_end      = mode->hsync_start + genmode->horizontal[3];
    mode->htotal         = mode->hsync_end + genmode->horizontal[4];

    mode->vdisplay       = genmode->vertical[1];
    mode->vsync_start    = mode->vdisplay + genmode->vertical[2];
    mode->vsync_end      = mode->vsync_start + genmode->vertical[3];
    mode->vtotal         = mode->vsync_end + genmode->vertical[4];

    mode->width_mm       = ctx->size.width;
    mode->height_mm      = ctx->size.height;

    // All tested panels (R36s orig, new-3, new-4; rgb10s) either require no sync or work OK with it.
    // So for now just always set no sync drm flags
    // TODO: pass drm flags in mode line if some future panel requires that
    mode->flags          = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
}

static int generic_panel_get_modes(struct drm_panel *panel,
                struct drm_connector *connector)
{
    struct generic_panel *ctx = panel_to_generic_panel(panel);
    struct drm_display_mode mode_tmp;
    struct drm_display_mode *mode;
    struct generic_panel_mode *genmode = ctx->modes;

    ctx->connector = connector;

    while (genmode) {
        dev_dbg(ctx->dev, "gen mode %d %dx%d\n", genmode->clock, genmode->horizontal[1], genmode->vertical[1]);

        generic_panel_fill_mode(ctx, genmode, &mode_tmp);

        mode = drm_mode_duplicate(connector->dev, &mode_tmp);
        if (!mode) {
            dev_err(ctx->dev, "Failed to add mode\n");
            return -ENOMEM;
        }
        drm_mode_set_name(mode);

        mode->type = DRM_MODE_TYPE_DRIVER;
        if (genmode->is_default) { mode->type |= DRM_MODE_TYPE_PREFERRED; };

        drm_mode_probed_add(connector, mode);

        genmode = genmode->prev;
    }

    connector->display_info.width_mm = ctx->size.width;
    connector->display_info.height_mm = ctx->size.height;


    /*
     * TODO: Remove once all drm drivers call
     * drm_connector_set_orientation_from_panel()
     */
    drm_connector_set_panel_orientation(connector, ctx->orientation);

    return 1;
}

/*
 * ArkOS timing switcher: "timings" lists the modes and "timing" reads or
 * sets the active index, as panel-simple did on the BSP kernels. ES's
 * refresh rate menu uses these files. Indexes follow panel_description.
 */
#define GENERIC_MATCH (DRM_MODE_MATCH_TIMINGS | DRM_MODE_MATCH_CLOCK | DRM_MODE_MATCH_FLAGS)

static int generic_panel_num_modes(struct generic_panel *ctx)
{
    struct generic_panel_mode *m;
    int n = 0;

    for (m = ctx->modes; m; m = m->prev)
        n++;
    return n;
}

/* ctx->modes is the last line of panel_description, ->prev walks back */
static const struct generic_panel_mode *generic_panel_mode_at(struct generic_panel *ctx, int index)
{
    struct generic_panel_mode *m = ctx->modes;
    int i;

    for (i = generic_panel_num_modes(ctx) - 1; m && i > index; i--)
        m = m->prev;
    return m;
}

static int generic_panel_current_timing(struct generic_panel *ctx)
{
    struct drm_connector *connector = ctx->connector;
    struct drm_crtc *crtc = connector && connector->state ? connector->state->crtc : NULL;
    struct drm_display_mode mode;
    int i, n = generic_panel_num_modes(ctx);

    if (crtc && crtc->state && crtc->state->enable) {
        for (i = 0; i < n; i++) {
            generic_panel_fill_mode(ctx, generic_panel_mode_at(ctx, i), &mode);
            if (drm_mode_match(&crtc->state->mode, &mode, GENERIC_MATCH))
                return i;
        }
    }
    return ctx->cur_timing;
}

static int generic_panel_commit_mode(struct drm_connector *connector,
                    const struct drm_display_mode *mode)
{
    struct drm_device *drm = connector->dev;
    struct drm_modeset_acquire_ctx actx;
    struct drm_connector_state *conn_state;
    struct drm_crtc_state *crtc_state;
    struct drm_atomic_commit *state;
    int ret;

    state = drm_atomic_commit_alloc(drm);
    if (!state)
        return -ENOMEM;

    drm_modeset_acquire_init(&actx, 0);
    state->acquire_ctx = &actx;
retry:
    conn_state = drm_atomic_get_connector_state(state, connector);
    if (IS_ERR(conn_state)) {
        ret = PTR_ERR(conn_state);
        goto out;
    }
    if (!conn_state->crtc) {
        ret = -ENODEV;
        goto out;
    }

    crtc_state = drm_atomic_get_crtc_state(state, conn_state->crtc);
    if (IS_ERR(crtc_state)) {
        ret = PTR_ERR(crtc_state);
        goto out;
    }
    if (!crtc_state->enable) {
        ret = -ENODEV;
        goto out;
    }

    ret = drm_atomic_set_mode_for_crtc(crtc_state, mode);
    if (!ret)
        ret = drm_atomic_commit(state);
out:
    if (ret == -EDEADLK) {
        drm_atomic_commit_clear(state);
        drm_modeset_backoff(&actx);
        goto retry;
    }
    drm_atomic_commit_put(state);
    drm_modeset_drop_locks(&actx);
    drm_modeset_acquire_fini(&actx);

    return ret;
}

/* Later KMS clients pick the preferred mode, so it follows the switch */
static void generic_panel_set_preferred(struct drm_connector *connector,
                    const struct drm_display_mode *mode)
{
    struct drm_display_mode *m;

    mutex_lock(&connector->dev->mode_config.mutex);
    list_for_each_entry(m, &connector->modes, head) {
        if (drm_mode_match(m, mode, GENERIC_MATCH))
            m->type |= DRM_MODE_TYPE_PREFERRED;
        else
            m->type &= ~DRM_MODE_TYPE_PREFERRED;
    }
    mutex_unlock(&connector->dev->mode_config.mutex);
}

/* fbdev restores the mode it chose at boot when it takes the display back */
static void generic_panel_update_fbdev_mode(struct drm_connector *connector,
                    const struct drm_display_mode *mode)
{
    struct drm_fb_helper *fb_helper = connector->dev->fb_helper;
    struct drm_mode_set *modeset;
    struct drm_display_mode *dup;
    unsigned int i;

    if (!fb_helper)
        return;

    mutex_lock(&fb_helper->client.modeset_mutex);
    drm_client_for_each_modeset(modeset, &fb_helper->client) {
        for (i = 0; i < modeset->num_connectors; i++) {
            if (modeset->connectors[i] != connector || !modeset->mode)
                continue;
            dup = drm_mode_duplicate(connector->dev, mode);
            if (!dup)
                break;
            drm_mode_destroy(connector->dev, modeset->mode);
            modeset->mode = dup;
            break;
        }
    }
    mutex_unlock(&fb_helper->client.modeset_mutex);
}

static ssize_t timings_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct generic_panel *ctx = dev_get_drvdata(dev);
    int i, n = generic_panel_num_modes(ctx), cur = generic_panel_current_timing(ctx);
    struct drm_display_mode mode;
    ssize_t len;

    len = sysfs_emit(buf, "count: %d\n", n);
    for (i = 0; i < n; i++) {
        generic_panel_fill_mode(ctx, generic_panel_mode_at(ctx, i), &mode);
        len += sysfs_emit_at(buf, len, "%d: %dx%d@%dHz%s\n", i, mode.hdisplay,
                     mode.vdisplay, drm_mode_vrefresh(&mode),
                     i == cur ? " (current)" : "");
    }
    return len;
}

static ssize_t timing_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "%d\n", generic_panel_current_timing(dev_get_drvdata(dev)));
}

static ssize_t timing_store(struct device *dev, struct device_attribute *attr,
                const char *buf, size_t count)
{
    struct generic_panel *ctx = dev_get_drvdata(dev);
    struct drm_connector *connector = ctx->connector;
    struct drm_display_mode mode;
    unsigned int index;
    int ret;

    ret = kstrtouint(buf, 0, &index);
    if (ret)
        return ret;
    if (index >= generic_panel_num_modes(ctx))
        return -ERANGE;
    if (!connector)
        return -ENODEV;

    generic_panel_fill_mode(ctx, generic_panel_mode_at(ctx, index), &mode);
    drm_mode_set_name(&mode);

    ret = generic_panel_commit_mode(connector, &mode);
    if (ret)
        return ret;

    ctx->cur_timing = index;
    generic_panel_set_preferred(connector, &mode);
    generic_panel_update_fbdev_mode(connector, &mode);

    return count;
}

static DEVICE_ATTR_RO(timings);
static DEVICE_ATTR_RW(timing);

static struct attribute *generic_panel_timing_attrs[] = {
    &dev_attr_timings.attr,
    &dev_attr_timing.attr,
    NULL,
};

static const struct attribute_group generic_panel_timing_group = {
    .attrs = generic_panel_timing_attrs,
};

static enum drm_panel_orientation generic_panel_get_orientation(struct drm_panel *panel)
{
    struct generic_panel *ctx = panel_to_generic_panel(panel);

    return ctx->orientation;
}

static const struct drm_panel_funcs generic_panel_funcs = {
    .unprepare  = generic_panel_unprepare,
    .prepare    = generic_panel_prepare,
    .get_modes  = generic_panel_get_modes,
    .get_orientation = generic_panel_get_orientation,
};

static int generic_panel_probe(struct mipi_dsi_device *dsi)
{
    struct device *dev = &dsi->dev;
    struct generic_panel *ctx;
    int ret;

    // Some defaults
    dsi->lanes = 1;
    dsi->format = MIPI_DSI_FMT_RGB888;
    dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
              MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_NO_EOT_PACKET |
              MIPI_DSI_CLOCK_NON_CONTINUOUS;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(7, 2, 0))
    ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);

    mipi_dsi_set_drvdata(dsi, ctx);

    ctx->dev = dev;

    ret = load_panel_description(dsi, ctx);
    if (ret < 0) {
        dev_err(dev, "Failed to load panel description\n");
        return ret;
    }

    mipi_dsi_set_drvdata(dsi, ctx);

    dev_info(dev, "lanes %d, format %d, mode %lx\n", dsi->lanes, dsi->format, dsi->mode_flags);

    drm_panel_init(&ctx->panel, &dsi->dev, &generic_panel_funcs,
               DRM_MODE_CONNECTOR_DSI);
#else
    ctx = devm_drm_panel_alloc(&dsi->dev, __typeof(*ctx), panel,
		    &generic_panel_funcs, DRM_MODE_CONNECTOR_DSI);

    if (IS_ERR(ctx))
        return PTR_ERR(ctx);

    ret = load_panel_description(dsi, ctx);
    if (ret < 0) {
        dev_err(dev, "Failed to load panel description\n");
        return ret;
    }

    mipi_dsi_set_drvdata(dsi, ctx);

    ctx->dev = dev;
    dev_info(dev, "lanes %d, format %d, mode %lx\n", dsi->lanes, dsi->format, dsi->mode_flags);
#endif

    if (!ctx)
        return -ENOMEM;

    ctx->enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
    if (IS_ERR(ctx->enable_gpio)) {
        dev_err(dev, "cannot get enable gpio\n");
	ctx->enable_gpio = NULL;
    }

    ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
    if (IS_ERR(ctx->reset_gpio)) {
        dev_err(dev, "cannot get reset gpio\n");
        return PTR_ERR(ctx->reset_gpio);
    }

    ctx->vdd = devm_regulator_get(dev, "vdd");
    if (IS_ERR(ctx->vdd)) {
        ret = PTR_ERR(ctx->vdd);
        if (ret != -EPROBE_DEFER)
            dev_err(dev, "Failed to request vdd regulator: %d\n", ret);
        return ret;
    }

    ctx->iovcc = devm_regulator_get(dev, "iovcc");
    if (IS_ERR(ctx->iovcc)) {
        ret = PTR_ERR(ctx->iovcc);
        if (ret != -EPROBE_DEFER)
            dev_err(dev, "Failed to request iovcc regulator: %d\n", ret);
        return ret;
    }

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(7, 3, 0))
    ret = drm_of_get_panel_orientation(dev->of_node, &ctx->orientation);
#else
    ret = of_drm_get_panel_orientation(dev->of_node, &ctx->orientation);
#endif
    if (ret < 0) {
        dev_err(dev, "%pOF: failed to get orientation %d\n", dev->of_node, ret);
        return ret;
    }

    ret = drm_panel_of_backlight(&ctx->panel);
    if (ret)
        return ret;

    drm_panel_add(&ctx->panel);

    ret = devm_device_add_group(dev, &generic_panel_timing_group);
    if (ret)
        dev_warn(dev, "no timing sysfs files: %d\n", ret);

    ret = mipi_dsi_attach(dsi);
    if (ret < 0) {
        dev_err(dev, "mipi_dsi_attach failed: %d\n", ret);
        drm_panel_remove(&ctx->panel);
        return ret;
    }

    return 0;
}

static void generic_panel_shutdown(struct mipi_dsi_device *dsi)
{
    struct generic_panel *ctx = mipi_dsi_get_drvdata(dsi);

    drm_panel_unprepare(&ctx->panel);

    drm_panel_disable(&ctx->panel);
}

static void generic_panel_remove(struct mipi_dsi_device *dsi)
{
    struct generic_panel *ctx = mipi_dsi_get_drvdata(dsi);
    int ret;

    generic_panel_shutdown(dsi);

    ret = mipi_dsi_detach(dsi);
    if (ret < 0)
        dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

    drm_panel_remove(&ctx->panel);
}

static const struct of_device_id generic_panel_of_match[] = {
    { .compatible = "rocknix,generic-dsi" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, generic_panel_of_match);

static struct mipi_dsi_driver generic_panel_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = generic_panel_of_match,
    },
    .probe  = generic_panel_probe,
    .remove = generic_panel_remove,
    .shutdown = generic_panel_shutdown,
};
module_mipi_dsi_driver(generic_panel_driver);

MODULE_AUTHOR("Danil Zagoskin <z@gosk.in>");
MODULE_DESCRIPTION("DRM driver for generic MIPI DSI panel");
MODULE_LICENSE("GPL v2");
