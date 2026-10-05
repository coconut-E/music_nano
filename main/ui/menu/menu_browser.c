#include <string.h>
#include <stdio.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "lvgl.h"
#include "sys_monitor.h"
#include "ui_res.h"
#include "ui_player.h"
#include "ui_novel.h"
#include "song_hash.h"
#include "likes.h"
#include "menu.h"

extern const lv_font_t lv_font_global_16;

#define FS_W        135    /* 面板宽 */
#define FS_H        220    /* 面板高 */
#define FS_X        0      /* 面板 X (从左上角展开) */
#define FS_Y        3      /* 面板 Y */
#define FS_ITEMS    30     /* 每页条目数 */

/* 文件浏览器控件句柄 */
static lv_obj_t  *s_fs_overlay   = NULL;  /* 全屏透明遮罩 */
static lv_obj_t  *s_fs_cont      = NULL;  /* 白色面板容器 */
static lv_obj_t  *s_fs_title     = NULL;  /* 标题 */
static lv_obj_t  *s_fs_list      = NULL;  /* 文件列表 */
static lv_obj_t  *s_fs_page_lbl  = NULL;  /* 页码标签 */
static lv_obj_t  *s_fs_prev_btn  = NULL;  /* 上一页按钮 */
static lv_obj_t  *s_fs_next_btn  = NULL;  /* 下一页按钮 */

static int   s_fs_page            = 0;     /* 当前页 */
static int   s_fs_total           = 0;     /* 总页数 */
static bool  s_fs_inside          = false; /* 是否已进入子目录 (目前仅赋值, 未被读取: 遗留状态) */
static const char *s_fs_group    = NULL;   /* 当前分组 (sdcard / sdcard/a/b) */
static lv_obj_t *s_fs_current_btn = NULL; /* 当前播放歌曲对应的列表行按钮 */

/* 列表行控件池 (复用): 只增不减, 上限 FS_ITEMS 行, 多余的行隐藏 */
static lv_obj_t *s_fs_rows[FS_ITEMS];   /* 行句柄 */
static int        s_fs_row_cnt = 0;     /* 已创建行数 */

/* 常驻面板状态: 控件只建一次, 关闭=隐藏 (LV_OBJ_FLAG_HIDDEN 即整棵子树退出渲染) */
static bool  s_fs_created = false;   /* 控件是否已创建 (仅全部步骤完成才置 true) */
static bool  s_fs_visible = false;   /* 面板是否展开 (替代 s_fs_overlay 存在性判断) */

/* 分步预建进度: LVGL 是单线程, 不能开任务建控件, 只能在本任务里分片执行。
 * 开机时一次性建好面板 (overlay/容器/列表 + 30 行) 会阻塞 ~100ms,
 * 拆成带时间预算的多步后, 每片只占几毫秒, 卡顿被开机背光渐入 (~300ms) 掩盖。 */
enum {
    FS_PC_IDLE = 0,   /* 图标描述符 */
    FS_PC_OVERLAY,    /* 全屏遮罩 */
    FS_PC_CONT,       /* 白色面板容器 */
    FS_PC_TITLE,      /* 标题 + 返回按钮 */
    FS_PC_LIST,       /* 文件列表 */
    FS_PC_NAV,        /* 上一页/页码/下一页 */
    FS_PC_ROWS,       /* 预建 30 个空行 (按预算分批) */
    FS_PC_DONE,       /* 收尾 */
};
static int   s_fs_pc_step = FS_PC_IDLE;   /* 当前预建阶段 */

/* 已构建状态: 打开时据此判断能否跳过重建 */
static char  s_fs_built_group[FS_GROUP_MAX];   /* 已构建的 group */
static int   s_fs_built_page  = -1;            /* 已构建的 page */
static char  s_fs_built_name[FS_NAME_MAX];     /* 已构建时高亮的曲名 (无则空串) */
static bool  s_fs_built_valid = false;         /* 是否已有有效构建 */

/* 当前播放信息快照 (show_page 期间缓存, 避免每行 O(N) 查询) */
static const char *s_cur_group = NULL;
static const char *s_cur_name  = NULL;

static lv_img_dsc_t s_fs_icon_dir;    /* 文件夹图标 */
static lv_img_dsc_t s_fs_icon_music;  /* 音乐文件图标 */
static lv_img_dsc_t s_fs_icon_heart;  /* 已喜欢文件图标 (粉色爱心) */
static lv_img_dsc_t s_fs_icon_novel;  /* 小说文件图标 (TXT) */

static void (*s_play_cb)(const char *group, const char *name) = NULL;   /* 音乐: 点击播放回调 */
static void (*s_novel_cb)(const char *group, const char *name) = NULL;  /* 小说: 点击打开回调 */

/* 当前浏览来源: 0=音乐库, 1=小说库 (决定使用的缓存/根分组/点击行为) */
static int s_fs_src = 0;

/* 当前来源对应的文件缓存 */
static fs_cache_t *fs_cache_active(void)
{
    return (s_fs_src == 1) ? g_novel_cache : g_fs_cache;
}

/* 当前来源的根分组 (用真实 '/', 与扫描器一致) */
static const char *fs_root_group(void)
{
    return (s_fs_src == 1) ? "sdcard/小说" : "sdcard/音乐";
}

/* 当前来源的根标题 */
static const char *fs_root_title(void)
{
    return (s_fs_src == 1) ? "Novel Files" : "Music Files";
}

/* 在缓存中查找 group 下 name 的顺序号 (含目录项, 与列表顺序一致); 未找到返回 -1 */
static int fs_cache_index_of(fs_cache_t *cache, const char *group, const char *name)
{
    if (!cache || !group || !name) return -1;
    int seen = 0;
    for (int i = 0; i < cache->count; i++) {
        fs_entry_t *e = &cache->entries[i];
        if (strcmp(e->group, group) != 0) continue;
        if (strcmp(e->name, name) == 0) return seen;
        seen++;
    }
    return -1;
}

static void fs_browser_create(void);
static void fs_browser_open(void);
static void fs_browser_close(void);
static void fs_browser_update(void);
static void fs_browser_rebuild(const char *group, int page, const char *name);
static void fs_browser_move_highlight(const char *name);
static void fs_browser_show_page(int page);
static void fs_browser_scroll_to_row(const char *name);
static void fs_browser_enter_dir(const char *parent_group, const char *name);
static void fs_browser_go_back(void);

/* 当前分组缓冲: s_fs_group 始终指向它, 便于拼接/回退 */
static char s_group_buf[FS_GROUP_MAX];

/* 设置当前分组 (拷贝进 s_group_buf); g=NULL 表示无分组 */
static void fs_set_group(const char *g)
{
    if (!g) {
        s_fs_group = NULL;
        return;
    }
    size_t n = strnlen(g, sizeof(s_group_buf) - 1);
    memcpy(s_group_buf, g, n);
    s_group_buf[n] = '\0';
    s_fs_group = s_group_buf;
}

/* 由父 group + 子目录名拼出子 group: parent/name */
static void fs_child_group(const char *parent, const char *name,
                           char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s", parent, name);
}

/* group 的显示名 (最后一段); 根或无返回当前来源标题 */
static const char *fs_group_display_name(const char *group)
{
    if (!group) return fs_root_title();
    const char *last = strrchr(group, '/');
    return last ? last + 1 : fs_root_title();
}

/* 注册点击播放回调: cb=播放函数 */
void fs_list_set_play_cb(void (*cb)(const char *group, const char *name))
{
    s_play_cb = cb;
}

/* 注册小说打开回调: cb=打开函数 */
void fs_list_set_novel_cb(void (*cb)(const char *group, const char *name))
{
    s_novel_cb = cb;
}

/* 切换浏览来源 (音乐/小说); 使已构建状态失效, 下次打开按新来源重建 */
void fs_browser_set_source(int src)
{
    if (s_fs_src == src) return;
    s_fs_src = src;
    s_fs_built_valid = false;
}

/* 关闭浏览器面板 (模式切换时调用, 不走动画) */
void fs_browser_close_panel(void)
{
    fs_browser_close();
}

/* 列表项点击: 目录→进入, 文件→播放 */
static void fs_item_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    fs_entry_t *entry = (fs_entry_t *)lv_obj_get_user_data(btn);   /* 条目存 user_data */
    if (!entry) return;

    if (entry->is_dir) {
        fs_browser_enter_dir(entry->group, entry->name);   /* 进入子目录 */
    } else if (s_fs_src == 1) {
        /* 小说: 关闭面板后打开阅读界面 */
        if (s_novel_cb && s_fs_group) {
            char group[FS_GROUP_MAX];
            size_t gl = strnlen(s_fs_group, sizeof(group) - 1);
            memcpy(group, s_fs_group, gl);
            group[gl] = '\0';
            char name[FS_NAME_MAX];
            size_t nl = strnlen(entry->name, sizeof(name) - 1);
            memcpy(name, entry->name, nl);
            name[nl] = '\0';
            fs_browser_close();
            s_novel_cb(group, name);
        }
    } else if (s_play_cb) {
        /* 播放切换后由 fs_browser_refresh() 统一重绘, 让绿色高亮跟随新播放的歌曲 */
        s_play_cb(s_fs_group, entry->name);
    }
}

/* 该列表项是否对应主界面当前正在播放的歌曲 */
static bool fs_entry_is_current(fs_entry_t *entry)
{
    if (s_fs_src == 1) return false;   /* 小说库不做当前播放高亮 */
    const char *group = s_cur_group;   /* show_page 开始时的快照, 避免每行 O(N) */
    const char *name  = s_cur_name;
    if (!group || !name || !entry || !s_fs_group) return false;

    /* 文件条目: 组与文件名都匹配 */
    if (!entry->is_dir) {
        return strcmp(s_fs_group, group) == 0 && strcmp(entry->name, name) == 0;
    }

    /* 目录条目: 当前播放歌曲所在目录是其子孙 → 高亮该文件夹 */
    char child[FS_GROUP_MAX];
    fs_child_group(entry->group, entry->name, child, sizeof(child));
    size_t cl = strlen(child);
    return strncmp(group, child, cl) == 0
           && (group[cl] == '\0' || group[cl] == '/');
}

/* 创建一行控件 (固定样式只设一次); 始终带 icon, 保证 child0=img child1=label */
static lv_obj_t *fs_row_create(void)
{
    lv_obj_t *btn = lv_list_add_btn(s_fs_list, &s_fs_icon_music, "");
    lv_obj_set_height(btn, 30);   /* 行高固定 30 (原 FS_ROW_H 宏未使用, 已删除) */
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_pad_top(btn, 3, 0);

    lv_obj_t *label = lv_obj_get_child(btn, 1);
    lv_obj_set_style_text_font(label, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_height(label, lv_font_get_line_height(&lv_font_global_16));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    lv_obj_add_event_cb(btn, fs_item_click_cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

/* 复用一行: 只更新图标/文件名/条目指针/高亮; entry=NULL 表示空目录提示行 */
static void fs_row_set(lv_obj_t *btn, fs_entry_t *entry)
{
    lv_obj_t *icon  = lv_obj_get_child(btn, 0);
    lv_obj_t *label = lv_obj_get_child(btn, 1);

    if (!entry) {   /* 空目录提示: 隐藏图标, 点击无效 */
        lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(label, (s_fs_src == 1) ? "无小说文件" : "无音乐文件");
        lv_obj_set_user_data(btn, NULL);
        lv_obj_remove_local_style_prop(btn, LV_STYLE_BG_COLOR, 0);
        lv_obj_remove_local_style_prop(btn, LV_STYLE_BG_OPA, 0);
        return;
    }

    lv_obj_clear_flag(icon, LV_OBJ_FLAG_HIDDEN);
    if (entry->is_dir) {
        lv_img_set_src(icon, &s_fs_icon_dir);
    } else if (s_fs_src == 1) {
        /* 小说库: 统一 TXT 文件图标 */
        lv_img_set_src(icon, &s_fs_icon_novel);
    } else {
        /* 音乐库: 已喜欢的文件显示粉色爱心, 其余用音乐图标 */
        char key[160];
        song_hash_name_key(entry->name, key, sizeof(key));
        bool liked = likes_contains(song_hash32(key, strlen(key)));
        lv_img_set_src(icon, liked ? &s_fs_icon_heart : &s_fs_icon_music);
    }
    lv_label_set_text(label, entry->name);
    lv_obj_set_user_data(btn, entry);   /* 存条目指针供点击回调 */

    /* 高亮当前正在播放的歌曲行 */
    if (fs_entry_is_current(entry)) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xB7F7C2), 0);   /* 淡绿高亮 */
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        s_fs_current_btn = btn;
    } else {
        lv_obj_remove_local_style_prop(btn, LV_STYLE_BG_COLOR, 0);
        lv_obj_remove_local_style_prop(btn, LV_STYLE_BG_OPA, 0);
    }
}

/* 记录本次构建状态 (供下次打开判断能否跳过重建) */
static void fs_built_record(int page)
{
    if (s_fs_group) {
        size_t gl = strnlen(s_fs_group, sizeof(s_fs_built_group) - 1);
        memcpy(s_fs_built_group, s_fs_group, gl);
        s_fs_built_group[gl] = '\0';
    } else {
        s_fs_built_group[0] = '\0';
    }
    s_fs_built_page = page;

    /* 高亮曲名: 仅当展示的就是当前歌曲所在组时才有意义 */
    s_fs_built_name[0] = '\0';
    if (s_fs_group && s_cur_group && s_cur_name &&
        strcmp(s_fs_group, s_cur_group) == 0) {
        size_t nl = strnlen(s_cur_name, sizeof(s_fs_built_name) - 1);
        memcpy(s_fs_built_name, s_cur_name, nl);
        s_fs_built_name[nl] = '\0';
    }
    s_fs_built_valid = true;
}

/* 显示指定页: 计算页数/页码, 复用行池更新列表 (不足补建, 多余隐藏) */
static void fs_browser_show_page(int page)
{
    s_fs_page = page;
    s_fs_current_btn = NULL;

    /* 缓存当前播放信息快照 (供 fs_entry_is_current 使用, 避免每行 O(N) 查询); 小说库无高亮 */
    if (s_fs_src == 1) {
        s_cur_group = NULL;
        s_cur_name  = NULL;
    } else {
        s_cur_group = player_current_group();
        s_cur_name  = player_current_name();
    }

    int64_t t0 = esp_timer_get_time();

    /* 单趟遍历: 同时统计 total 并收集本页条目 (原为每行各扫一趟全表) */
    int total = 0;
    int start = page * FS_ITEMS;   /* 本页首条目 */
    int end   = start + FS_ITEMS;
    fs_entry_t *page_entries[FS_ITEMS];
    int page_n = 0;

    fs_cache_t *cache = fs_cache_active();
    if (cache && s_fs_group) {
        for (int i = 0; i < cache->count; i++) {
            fs_entry_t *e = &cache->entries[i];
            if (strcmp(e->group, s_fs_group) != 0) continue;
            if (total >= start && total < end) page_entries[page_n++] = e;
            total++;
        }
    }

    int64_t t_scan = esp_timer_get_time() - t0;

    s_fs_total = (total + FS_ITEMS - 1) / FS_ITEMS;   /* 总页数 (向上取整) */
    if (s_fs_total < 1) s_fs_total = 1;

    char buf[32];
    snprintf(buf, sizeof(buf), "%d/%d", page + 1, s_fs_total);   /* 页码 */
    lv_label_set_text(s_fs_page_lbl, buf);

    /* 首页/末页禁用对应翻页按钮 */
    if (page <= 0) {
        lv_obj_add_state(s_fs_prev_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(s_fs_prev_btn, LV_STATE_DISABLED);
    }
    if (page >= s_fs_total - 1) {
        lv_obj_add_state(s_fs_next_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(s_fs_next_btn, LV_STATE_DISABLED);
    }

    /* 需要多少行: 空目录也要 1 行显示提示; 上限 FS_ITEMS (更多靠翻页) */
    int need = (total == 0) ? 1 : page_n;

    /* 行池只增不减: 不足则补建 (已存在的行不销毁) */
    int created = 0;
    while (s_fs_row_cnt < need) {
        s_fs_rows[s_fs_row_cnt++] = fs_row_create();
        created++;
    }

    /* 复用更新前 need 行, 隐藏多余行 */
    for (int i = 0; i < need; i++) {
        lv_obj_clear_flag(s_fs_rows[i], LV_OBJ_FLAG_HIDDEN);
        fs_row_set(s_fs_rows[i], (total == 0) ? NULL : page_entries[i]);
    }
    for (int i = need; i < s_fs_row_cnt; i++) {
        lv_obj_add_flag(s_fs_rows[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 重建列表后把滚动归零: 避免残留旧目录/旧内容的滚动量 (拔卡时尤其明显),
     * 否则视口会落在内容范围之外 → 列表显示空白, 滑动后被钳制到最底端.
     * 需要保留滚动量的调用方 (refresh) 在返回后自行恢复. */
    lv_obj_scroll_to_y(s_fs_list, 0, LV_ANIM_OFF);

    printf("[FS] show_page page=%d total=%d rows=%d(created %d) scan=%.2fms build=%.2fms\n",
           page, total, need, created, t_scan / 1000.0f,
           (esp_timer_get_time() - t0 - t_scan) / 1000.0f);
    fs_built_record(page);
}

/* 滚动到指定文件名所在行 (不高亮); name 为空则不动作 */
static void fs_browser_scroll_to_row(const char *name)
{
    if (!name || !name[0] || !s_fs_list) return;
    lv_obj_update_layout(s_fs_list);
    for (int i = 0; i < s_fs_row_cnt; i++) {
        lv_obj_t *btn = s_fs_rows[i];
        if (lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN)) continue;
        fs_entry_t *e = (fs_entry_t *)lv_obj_get_user_data(btn);
        if (e && !e->is_dir && strcmp(e->name, name) == 0) {
            lv_obj_scroll_to_view(btn, LV_ANIM_OFF);
            break;
        }
    }
}

/* 上一页 */
static void fs_prev_click_cb(lv_event_t *e)
{
    if (s_fs_page > 0) {
        fs_browser_show_page(s_fs_page - 1);
    }
}

/* 下一页 */
static void fs_next_click_cb(lv_event_t *e)
{
    if (s_fs_page < s_fs_total - 1) {
        fs_browser_show_page(s_fs_page + 1);
    }
}

/* 实际关闭浏览器: 仅隐藏 (控件常驻, 不删除) */
static void fs_browser_close(void)
{
    if (s_fs_overlay) {
        lv_obj_add_flag(s_fs_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    s_fs_visible = false;
}

static void fs_overlay_click_cb(lv_event_t *e)
{
        static const panel_anim_cfg_t cfg = {
            .overlay_ref = &s_fs_overlay,
            .cont_ref    = &s_fs_cont,
            .w = FS_W, .h = FS_H,
            .x = ANIM_FS_X, .y = ANIM_FS_Y,
            .anchor_right = false,
            .open_real  = fs_browser_open,
            .close_real = fs_browser_close,
        };
        panel_anim_close(&cfg);
}

static void fs_browser_enter_dir(const char *parent_group, const char *name)
{
    s_fs_inside = true;

    char child[FS_GROUP_MAX];
    fs_child_group(parent_group, name, child, sizeof(child));   /* 组名 = 父/子 (真实 '/') */
    fs_set_group(child);

    lv_label_set_text(s_fs_title, name);   /* 标题 = 目录名 */

    fs_browser_show_page(0);
}

/* 返回上一级目录 (根则不变) */
static void fs_browser_go_back(void)
{
    if (!s_fs_group) return;

    /* 截到最后一个 '/' 得到父 group */
    char parent[FS_GROUP_MAX];
    size_t n = strnlen(s_fs_group, sizeof(parent) - 1);
    memcpy(parent, s_fs_group, n);
    parent[n] = '\0';

    const char *root = fs_root_group();
    char *last = strrchr(parent, '/');
    if (last) *last = '\0';

    /* 截出的父分组必须仍在根之下, 否则钳到根 */
    if (strncmp(parent, root, strlen(root)) != 0 || strlen(parent) < strlen(root)) {
        fs_set_group(root);
    } else {
        fs_set_group(parent);
    }

    s_fs_inside = (strcmp(s_fs_group, root) != 0);
    lv_label_set_text(s_fs_title, fs_group_display_name(s_fs_group));
    fs_browser_show_page(0);
}

/* 返回按钮 */
static void fs_back_click_cb(lv_event_t *e)
{
    fs_browser_go_back();
}

/* 分步创建面板控件 (幂等, 可续跑): 单次调用最多耗时 budget_us 微秒, 返回 true=尚未完成。
 * budget_us <= 0 表示不限时, 一次做完 (打开路径与内部兜底使用)。
 *
 * 为什么分步: LVGL 是单线程, 30 个空行 + 面板控件一次性创建会阻塞 ~100ms。
 * 拆成带时间预算的多步后, 每片只占几毫秒, 开机时由 ui_loop 在背光渐入动画期间分片跑完,
 * 把这段卡顿藏进渐入里, 避免启动时一次性停顿。 */
bool fs_browser_precreate_step(int budget_us)
{
    if (s_fs_created) return false;   /* 已建好: 直接返回, 调用方每轮调用也几乎零开销 */

    int64_t t0 = esp_timer_get_time();

    while (!s_fs_created) {
        switch (s_fs_pc_step) {
        case FS_PC_IDLE:
            /* 加载文件夹/音乐图标描述符 (纯赋值, 很快) */
            s_fs_icon_dir.header.w = 16;
            s_fs_icon_dir.header.h = 21;
            s_fs_icon_dir.data_size = 16 * 21 * 2;
            s_fs_icon_dir.header.cf = LV_IMG_CF_TRUE_COLOR;
            s_fs_icon_dir.data = (const uint8_t *)icon_file[0];
            s_fs_icon_music.header.w = 16;
            s_fs_icon_music.header.h = 21;
            s_fs_icon_music.data_size = 16 * 21 * 2;
            s_fs_icon_music.header.cf = LV_IMG_CF_TRUE_COLOR;
            s_fs_icon_music.data = (const uint8_t *)icon_file[1];
            s_fs_icon_heart.header.w = 16;
            s_fs_icon_heart.header.h = 21;
            s_fs_icon_heart.data_size = 16 * 21 * 2;
            s_fs_icon_heart.header.cf = LV_IMG_CF_TRUE_COLOR;
            s_fs_icon_heart.data = (const uint8_t *)icon_heart;
            s_fs_icon_novel.header.w = 16;
            s_fs_icon_novel.header.h = 21;
            s_fs_icon_novel.data_size = 16 * 21 * 2;
            s_fs_icon_novel.header.cf = LV_IMG_CF_TRUE_COLOR;
            s_fs_icon_novel.data = (const uint8_t *)icon_novel;
            s_fs_pc_step = FS_PC_OVERLAY;
            break;

        case FS_PC_OVERLAY:
            /* 全屏透明遮罩 */
            s_fs_overlay = lv_btn_create(lv_scr_act());
            lv_obj_set_size(s_fs_overlay, 172, 320);
            lv_obj_set_pos(s_fs_overlay, 0, 0);
            lv_obj_set_style_bg_opa(s_fs_overlay, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(s_fs_overlay, 0, 0);
            lv_obj_set_style_shadow_width(s_fs_overlay, 0, 0);
            lv_obj_add_event_cb(s_fs_overlay, fs_overlay_click_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_clear_flag(s_fs_overlay, LV_OBJ_FLAG_SCROLLABLE);
            /* 关键: 建完立刻隐藏。分步执行期间遮罩会短暂存在,
             * 若不隐藏则既参与渲染又可能误吞触摸, 且 panel_anim 截主界面时会拍到它。 */
            lv_obj_add_flag(s_fs_overlay, LV_OBJ_FLAG_HIDDEN);
            s_fs_pc_step = FS_PC_CONT;
            break;

        case FS_PC_CONT:
            /* 白色面板容器 */
            s_fs_cont = lv_obj_create(s_fs_overlay);
            lv_obj_set_pos(s_fs_cont, FS_X, FS_Y);
            lv_obj_set_size(s_fs_cont, FS_W, FS_H);
            lv_obj_set_style_bg_color(s_fs_cont, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(s_fs_cont, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(s_fs_cont, 4, 0);
            lv_obj_set_style_border_color(s_fs_cont, lv_color_hex(0xCCCCCC), 0);
            lv_obj_set_style_border_width(s_fs_cont, 1, 0);
            lv_obj_set_style_pad_all(s_fs_cont, 0, 0);
            lv_obj_clear_flag(s_fs_cont, LV_OBJ_FLAG_SCROLLABLE);
            s_fs_pc_step = FS_PC_TITLE;
            break;

        case FS_PC_TITLE: {
            /* 标题 */
            s_fs_title = lv_label_create(s_fs_cont);
            lv_obj_set_pos(s_fs_title, 4, 0);
            lv_obj_set_size(s_fs_title, FS_W - 40, 22);
            lv_obj_set_style_text_font(s_fs_title, &lv_font_global_16, 0);
            lv_obj_set_style_text_color(s_fs_title, lv_color_black(), 0);
            lv_label_set_long_mode(s_fs_title, LV_LABEL_LONG_DOT);
            lv_label_set_text(s_fs_title, fs_root_title());

            /* 返回(上一级)按钮 */
            lv_obj_t *back_btn = lv_btn_create(s_fs_cont);
            lv_obj_set_pos(back_btn, FS_W - 36, 1);
            lv_obj_set_size(back_btn, 34, 16);
            lv_obj_set_style_radius(back_btn, 3, 0);
            lv_obj_set_style_bg_color(back_btn, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(back_btn, 0, 0);
            lv_obj_set_style_shadow_width(back_btn, 0, 0);
            lv_obj_add_event_cb(back_btn, fs_back_click_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_t *back_lbl = lv_label_create(back_btn);
            lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
            lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(back_lbl, lv_color_hex(0x0000FF), 0);
            lv_obj_center(back_lbl);
            s_fs_pc_step = FS_PC_LIST;
            break;
        }

        case FS_PC_LIST:
            /* 文件列表 */
            s_fs_list = lv_list_create(s_fs_cont);
            lv_obj_set_pos(s_fs_list, 0, 22);
            /* 修改列表高度：原为 FS_H - 36，现减少高度以为大按钮腾出空间 */
            /* 假设底部导航区高度设为 40 (原18)，则列表高度 = FS_H(220) - 22(标题) - 40(底部) = 158 */
            lv_obj_set_size(s_fs_list, FS_W, FS_H - 58);
            lv_obj_set_style_bg_color(s_fs_list, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(s_fs_list, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(s_fs_list, 0, 0);
            lv_obj_set_style_pad_all(s_fs_list, 0, 0);
            s_fs_pc_step = FS_PC_NAV;
            break;

        case FS_PC_NAV: {
            /* 底部导航区 Y */
            int nav_y = FS_H - 35;

            /* 上一页按钮 */
            s_fs_prev_btn = lv_btn_create(s_fs_cont);
            lv_obj_set_pos(s_fs_prev_btn, 2, nav_y);
            lv_obj_set_size(s_fs_prev_btn, 40, 32);
            lv_obj_set_style_radius(s_fs_prev_btn, 6, 0);
            lv_obj_set_style_bg_color(s_fs_prev_btn, lv_color_hex(0xE0E0E0), 0);
            lv_obj_set_style_bg_opa(s_fs_prev_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_shadow_width(s_fs_prev_btn, 0, 0);
            lv_obj_add_event_cb(s_fs_prev_btn, fs_prev_click_cb, LV_EVENT_CLICKED, NULL);

            lv_obj_t *prev_lbl = lv_label_create(s_fs_prev_btn);
            lv_label_set_text(prev_lbl, "<");
            lv_obj_set_style_text_font(prev_lbl, &lv_font_global_16, 0);
            lv_obj_set_style_text_color(prev_lbl, lv_color_hex(0x0000FF), 0);
            lv_obj_center(prev_lbl);

            /* 页码标签 */
            s_fs_page_lbl = lv_label_create(s_fs_cont);
            lv_obj_set_pos(s_fs_page_lbl, 47, nav_y + 8);
            lv_obj_set_size(s_fs_page_lbl, 40, 16);
            lv_obj_set_style_text_align(s_fs_page_lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(s_fs_page_lbl, &lv_font_global_16, 0);
            lv_obj_set_style_text_color(s_fs_page_lbl, lv_color_black(), 0);
            lv_label_set_text(s_fs_page_lbl, "1/1");

            /* 下一页按钮 */
            s_fs_next_btn = lv_btn_create(s_fs_cont);
            lv_obj_set_pos(s_fs_next_btn, FS_W - 42, nav_y);
            lv_obj_set_size(s_fs_next_btn, 40, 32);
            lv_obj_set_style_radius(s_fs_next_btn, 6, 0);
            lv_obj_set_style_bg_color(s_fs_next_btn, lv_color_hex(0xE0E0E0), 0);
            lv_obj_set_style_bg_opa(s_fs_next_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_shadow_width(s_fs_next_btn, 0, 0);
            lv_obj_add_event_cb(s_fs_next_btn, fs_next_click_cb, LV_EVENT_CLICKED, NULL);

            lv_obj_t *next_lbl = lv_label_create(s_fs_next_btn);
            lv_label_set_text(next_lbl, ">");
            lv_obj_set_style_text_font(next_lbl, &lv_font_global_16, 0);
            lv_obj_set_style_text_color(next_lbl, lv_color_hex(0x0000FF), 0);
            lv_obj_center(next_lbl);
            s_fs_pc_step = FS_PC_ROWS;
            break;
        }

        case FS_PC_ROWS:
            /* 预建 30 个空行控件 (隐藏): 首次打开/换目录时无需再创建, 只更新内容。
             * 这是整个创建过程最重的一步, 故每建一行都检查时间预算, 用完就返回, 下一轮续建。 */
            while (s_fs_row_cnt < FS_ITEMS) {
                lv_obj_t *row = fs_row_create();
                lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
                s_fs_rows[s_fs_row_cnt++] = row;
                if (budget_us > 0 && esp_timer_get_time() - t0 >= budget_us) {
                    return true;   /* 预算用完, 保留 s_fs_pc_step 待下次续跑 */
                }
            }
            s_fs_pc_step = FS_PC_DONE;
            break;

        case FS_PC_DONE:
            /* 全部步骤完成: 遮罩在创建时已隐藏, 这里只需置完成标志 */
            s_fs_created = true;
            //ESP_LOGI("LVGL", "文件浏览器 is OK"); 
            return false;
        }

        /* 每个阶段结束后检查预算: 超时就退出, 下一轮从当前阶段继续 */
        if (budget_us > 0 && esp_timer_get_time() - t0 >= budget_us) {
            return true;
        }
    }
    return false;
}

/* 创建面板控件 (幂等): 打开路径与内部兜底使用, 不限时一次建完 */
static void fs_browser_create(void)
{
    fs_browser_precreate_step(0);
}

/* 重建到指定 group/page: 设标题 → 重绘本页 → 定位到目标行
 * 音乐: 滚动到当前播放歌曲 (带高亮); 小说: 滚动到当前打开文件 (不高亮) */
static void fs_browser_rebuild(const char *group, int page, const char *name)
{
    fs_set_group(group);
    s_fs_inside = (strcmp(group, fs_root_group()) != 0);
    lv_label_set_text(s_fs_title, fs_group_display_name(group));
    fs_browser_show_page(page);

    if (s_fs_current_btn) {
        lv_obj_update_layout(s_fs_list);
        lv_obj_scroll_to_view(s_fs_current_btn, LV_ANIM_OFF);
    } else if (s_fs_src == 1) {
        fs_browser_scroll_to_row(name);
    }
}

/* 仅搬动绿色高亮 (同组同页, 只有当前歌曲变了): 不重建任何行 */
static void fs_browser_move_highlight(const char *name)
{
    if (s_fs_current_btn) {
        /* 移除本地 bg 样式, 恢复主题默认 (即取消高亮) */
        lv_obj_remove_local_style_prop(s_fs_current_btn, LV_STYLE_BG_COLOR, 0);
        lv_obj_remove_local_style_prop(s_fs_current_btn, LV_STYLE_BG_OPA, 0);
        s_fs_current_btn = NULL;
    }
    if (!name) return;

    for (int i = 0; i < s_fs_row_cnt; i++) {
        lv_obj_t *btn = s_fs_rows[i];
        if (lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN)) continue;
        fs_entry_t *e = (fs_entry_t *)lv_obj_get_user_data(btn);
        if (e && !e->is_dir && strcmp(e->name, name) == 0) {
            lv_obj_set_style_bg_color(btn, lv_color_hex(0xB7F7C2), 0);   /* 淡绿高亮 */
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            s_fs_current_btn = btn;
            break;
        }
    }
}

/* 打开时按需更新:
 *  (a) 目标与已构建状态完全一致 → 音乐什么都不做; 小说仍滚到当前文件;
 *  (b) 同组同页(音乐), 仅当前歌曲变了 → 只搬高亮;
 *  (c) 其余 (换目录/换页/首次) → 重建。 */
static void fs_browser_update(void)
{
    const char *group;
    const char *name = NULL;
    int  page = 0;
    char novel_group[FS_GROUP_MAX];
    char novel_name[FS_NAME_MAX];

    if (s_fs_src == 1) {
        /* 小说库: 定位到当前打开小说所在目录 (缓存中找不到则回退根目录) */
        fs_cache_t *cache = g_novel_cache;
        int pos = -1;
        if (ui_novel_current(novel_group, sizeof(novel_group), novel_name, sizeof(novel_name))
            && cache) {
            pos = fs_cache_index_of(cache, novel_group, novel_name);
        }
        if (pos >= 0) {
            group = novel_group;
            name  = novel_name;
            page  = pos / FS_ITEMS;
        } else {
            group = fs_root_group();
            name  = NULL;
            page  = 0;
        }
    } else {
        group = player_current_group();
        name  = player_current_name();
        fs_cache_t *cache = fs_cache_active();
        if (group && name && cache) {
            /* 在组内找到当前歌曲序号 → 页码 (含目录项, 与列表顺序一致) */
            int pos = fs_cache_index_of(cache, group, name);
            page = (pos >= 0) ? (pos / FS_ITEMS) : 0;   /* 未找到也停在该组首页 */
        } else {   /* 无当前歌曲: 根目录 */
            group = fs_root_group();
            name  = NULL;
            page  = 0;
        }
    }

    const char *bn = name ? name : "";
    bool same_group = s_fs_built_valid && strcmp(s_fs_built_group, group) == 0;
    bool same_page  = same_group && s_fs_built_page == page;

    if (same_page && strcmp(s_fs_built_name, bn) == 0) {
        printf("[FS] open update: skip (unchanged)\n");   /* (a) */
        if (s_fs_src == 1) fs_browser_scroll_to_row(name);  /* 小说每次打开都滚到当前文件 */
        return;
    }
    if (same_page && s_fs_src == 0) {
        printf("[FS] open update: highlight only\n");     /* (b) */
        fs_browser_move_highlight(name);
        size_t nl = strnlen(bn, sizeof(s_fs_built_name) - 1);
        memcpy(s_fs_built_name, bn, nl);
        s_fs_built_name[nl] = '\0';
        return;
    }

    printf("[FS] open update: rebuild\n");                /* (c) */
    fs_browser_rebuild(group, page, name);
}

/* 实际打开浏览器 (open_real): 确保控件存在 → 按需更新 → 显示 */
static void fs_browser_open(void)
{
    int64_t t0 = esp_timer_get_time();
    fs_browser_create();
    int64_t t1 = esp_timer_get_time();

    /* 先置顶并显示: 让布局立即生效, 之后的滚动定位 (scroll_to_view) 才准确.
     * 否则在隐藏状态下定位, 坐标未更新, 可能把列表滚到错误位置. */
    lv_obj_move_foreground(s_fs_overlay);
    lv_obj_clear_flag(s_fs_overlay, LV_OBJ_FLAG_HIDDEN);
    s_fs_visible = true;

    fs_browser_update();
    int64_t t2 = esp_timer_get_time();

    fs_cache_t *cache = fs_cache_active();
    printf("[FS] open create=%.2fms update=%.2fms cache_count=%d\n",
           (t1 - t0) / 1000.0f, (t2 - t1) / 1000.0f,
           cache ? cache->count : 0);
}

/* 两向同步: 播放歌曲变化时刷新绿色高亮。
 * 关闭状态: 什么都不做 (打开时用 group/page/name 比对决定走快路径还是重建, 不保温);
 * 打开状态: 当前分组是播放歌曲所在目录的祖先 (或相等) 时重绘, 保留滚动位置。 */
void fs_browser_refresh(void)
{
    if (s_fs_src == 1) return;   /* 小说库无当前播放高亮 */
    if (!s_fs_visible) return;   /* 关闭中: 交给打开时的比对 */
    if (!fs_cache_active() || !s_fs_group) return;

    const char *group = player_current_group();
    const char *name  = player_current_name();
    if (!group || !name) return;

    size_t sl = strlen(s_fs_group);
    bool in_view = (strcmp(s_fs_group, group) == 0)
                   || (strncmp(group, s_fs_group, sl) == 0 && group[sl] == '/');
    if (!in_view) return;

    lv_coord_t scroll_y = lv_obj_get_scroll_y(s_fs_list);
    fs_browser_show_page(s_fs_page);
    lv_obj_scroll_to_y(s_fs_list, scroll_y, LV_ANIM_OFF);
}

/* 喜欢状态变化: 让已构建列表失效 (下次打开重建), 可见则立即重绘爱心图标 */
void fs_browser_likes_changed(void)
{
    s_fs_built_valid = false;
    if (s_fs_visible) {
        lv_coord_t scroll_y = lv_obj_get_scroll_y(s_fs_list);   /* 仅图标变化: 保留滚动位置 */
        fs_browser_show_page(s_fs_page);
        lv_obj_scroll_to_y(s_fs_list, scroll_y, LV_ANIM_OFF);
    }
}

/* 导航到当前播放歌曲所在目录并高亮 (SD 恢复时使用) */
void fs_browser_jump(void)
{
    if (s_fs_src == 1) return;   /* 小说库无当前播放定位 */
    if (!s_fs_visible) return;   /* 关闭中: 交给下次打开时的比对 */
    fs_browser_update();
}

void fs_menu_click_cb(lv_event_t *e)
{
    if (s_fs_visible) {
        /* 已打开时再点一次: 关闭浏览器 */
        fs_browser_close();
    } else {
        /* 未打开: 打开浏览器 (带展开动画) */
        static const panel_anim_cfg_t cfg = {
            .overlay_ref = &s_fs_overlay,
            .cont_ref    = &s_fs_cont,
            .w = FS_W, .h = FS_H,
            .x = ANIM_FS_X, .y = ANIM_FS_Y,
            .anchor_right = false,
            .open_real  = fs_browser_open,
            .close_real = fs_browser_close,
        };
        panel_anim_open(&cfg);
    }
}

void fs_browser_on_sd_ready(void)
{
    s_fs_built_valid = false;   /* 缓存重载, 已构建行内容失效 */
    if (s_fs_visible) {
        fs_set_group(fs_root_group());
        s_fs_inside = false;
        lv_label_set_text(s_fs_title, fs_root_title());
        fs_browser_show_page(0);
    }
}

void fs_browser_on_sd_remove(void)
{
    s_fs_built_valid = false;
    if (s_fs_visible) {
        fs_set_group(NULL);
        fs_browser_show_page(0);
    }
}
