#ifndef __UI_NOVEL_H__
#define __UI_NOVEL_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 小说组生命周期 (由 app_mode 调度):
 *  - ui_novel_show: 构建并显示阅读界面 (懒建)
 *  - ui_novel_destroy: 关文件 + 删除全部控件 + 重置模块静态变量 */
void ui_novel_show(void);
void ui_novel_destroy(void);

/* 打开小说文件并显示第一页 (由文件浏览器选中回调调用). group/name=文件浏览器条目 */
void ui_novel_open(const char *group, const char *name);

/* SD 拔出: 关闭当前打开的小说文件 (不销毁 UI) */
void ui_novel_on_sd_remove(void);

/* 小说组是否处于活动状态 (已构建) */
bool ui_novel_is_active(void);

#ifdef __cplusplus
}
#endif

#endif
