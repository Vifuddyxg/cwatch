#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <dirent.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define APP_TITLE "cwatch"
#define WIN_W 1280
#define WIN_H 760
#define MAX_ITEMS 2048
#define MAX_COLLECTIONS 512
#define MAX_SECTIONS 512
#define MAX_VIEW_ITEMS (MAX_ITEMS + MAX_COLLECTIONS)
#define MAX_PLAYERS 8
#define MAX_EXTRA_ROOTS 32
#define CARD_W 260
#define CARD_H 190
#define THUMB_H 128
#define CARD_GAP 26
#define ROW_GAP 64
#define TOP_H 88
#define LEFT_PAD 38
#define TITLE_LEN 256
#define SECTION_LEN 128

typedef enum {
    VIEW_VIDEO = 0,
    VIEW_COLLECTION = 1
} ViewKind;

typedef struct {
    char path[PATH_MAX];
    char title[TITLE_LEN];
    char section[SECTION_LEN];
    int collection_idx;
    int episode_number;
    bool has_progress;
    double progress_sec;
    bool watched;
} MediaItem;

typedef struct {
    char name[SECTION_LEN];
    char path[PATH_MAX];
    int media_indices[MAX_ITEMS];
    int media_count;
    int first_media_idx;
    bool pinned;
} Collection;

typedef struct {
    ViewKind kind;
    int index;
    char section[SECTION_LEN];
} ViewItem;

typedef struct {
    char name[SECTION_LEN];
    int first;
    int count;
    int y;
} Section;

typedef struct {
    MediaItem items[MAX_ITEMS];
    SDL_Texture *thumbs[MAX_ITEMS];
    int item_count;

    Collection collections[MAX_COLLECTIONS];
    int collection_count;

    ViewItem view_items[MAX_VIEW_ITEMS];
    int view_count;
    Section sections[MAX_SECTIONS];
    int section_count;

    int selected;
    int scroll_y;
    int max_scroll;

    char root[PATH_MAX];
    char extra_roots[MAX_EXTRA_ROOTS][PATH_MAX];
    int extra_root_count;

    char pinned_paths[MAX_COLLECTIONS][PATH_MAX];
    int pinned_count;

    char active_collection_path[PATH_MAX];
    bool in_collection_view;

    char search[TITLE_LEN];
    bool search_active;
    char input_path[PATH_MAX];
    bool input_active;
    bool ignore_next_text;

    bool thumbs_loaded;
    int thumb_cursor;
} Library;

typedef struct {
    TTF_Font *sm;
    TTF_Font *md;
    TTF_Font *lg;
} UI;

typedef struct {
    char name[32];
    char exec[64];
} Player;

static const char *font_paths[] = {
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    NULL
};

static int win_w = WIN_W;
static int win_h = WIN_H;
static const Library *sort_lib = NULL;

static void copy_text(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    if (!src) src = "";
    size_t len = strnlen(src, dst_len - 1);
    memmove(dst, src, len);
    dst[len] = '\0';
}

static bool path_join(char *out, size_t out_len, const char *left, const char *right)
{
    if (!out || out_len == 0) return false;
    if (!left) left = "";
    if (!right) right = "";

    int written;
    size_t left_len = strlen(left);
    if (left_len > 0 && left[left_len - 1] == '/')
        written = snprintf(out, out_len, "%s%s", left, right);
    else
        written = snprintf(out, out_len, "%s/%s", left, right);

    if (written < 0 || (size_t)written >= out_len) {
        out[0] = '\0';
        return false;
    }
    return true;
}

static int natural_char_rank(unsigned char c)
{
    if (c == '\0') return -1;
    return tolower(c);
}

static int natural_compare(const char *a, const char *b)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;

    while (*pa || *pb) {
        if (isdigit(*pa) && isdigit(*pb)) {
            while (*pa == '0') pa++;
            while (*pb == '0') pb++;

            const unsigned char *na = pa;
            const unsigned char *nb = pb;
            while (isdigit(*na)) na++;
            while (isdigit(*nb)) nb++;

            int len_a = (int)(na - pa);
            int len_b = (int)(nb - pb);
            if (len_a != len_b) return len_a - len_b;

            int cmp = strncmp((const char *)pa, (const char *)pb, (size_t)len_a);
            if (cmp != 0) return cmp;

            pa = na;
            pb = nb;
            continue;
        }

        int ca = natural_char_rank(*pa);
        int cb = natural_char_rank(*pb);
        if (ca != cb) return ca - cb;
        if (*pa) pa++;
        if (*pb) pb++;
    }

    return 0;
}

static TTF_Font *load_font(int pt)
{
    for (int i = 0; font_paths[i]; i++) {
        TTF_Font *f = TTF_OpenFont(font_paths[i], pt);
        if (f) return f;
    }
    return NULL;
}

static bool command_exists(const char *cmd)
{
    const char *path = getenv("PATH");
    if (!path) return false;
    char copy[4096];
    copy_text(copy, sizeof(copy), path);
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char full[PATH_MAX];
        if (!path_join(full, sizeof(full), dir, cmd))
            continue;
        if (access(full, X_OK) == 0) return true;
    }
    return false;
}

static unsigned long hash_string(const char *s)
{
    unsigned long hash = 1469598103934665603UL;
    while (*s) {
        hash ^= (unsigned char)*s++;
        hash *= 1099511628211UL;
    }
    return hash;
}

static void ensure_thumb_dir(const Library *lib, char *out, size_t out_len)
{
    char cache_dir[PATH_MAX];
    if (!path_join(cache_dir, sizeof(cache_dir), lib->root, ".cache"))
        return;
    mkdir(cache_dir, 0755);
    if (!path_join(out, out_len, cache_dir, "thumbs"))
        return;
    mkdir(out, 0755);
}

static void ensure_state_dir(char *out, size_t out_len)
{
    const char *state_home = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    if (state_home && state_home[0] != '\0')
        path_join(out, out_len, state_home, "cwatch");
    else if (home && home[0] != '\0')
        snprintf(out, out_len, "%s/%s", home, ".local/state/cwatch");
    else
        copy_text(out, out_len, ".cwatch-state");

    mkdir(out, 0755);
}

static void ensure_watch_later_dir(char *out, size_t out_len)
{
    char state_dir[PATH_MAX];
    ensure_state_dir(state_dir, sizeof(state_dir));
    if (!path_join(out, out_len, state_dir, "watch_later"))
        return;
    mkdir(out, 0755);
}

static void watched_file_path(char *out, size_t out_len)
{
    char state_dir[PATH_MAX];
    ensure_state_dir(state_dir, sizeof(state_dir));
    path_join(out, out_len, state_dir, "watched.txt");
}

static int discover_players(Player *players)
{
    const Player candidates[] = {
        {"mpv", "mpv"},
        {"vlc", "vlc"},
        {"celluloid", "celluloid"},
        {"parole", "parole"},
        {"xdg-open", "xdg-open"}
    };
    int count = 0;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (command_exists(candidates[i].exec) && count < MAX_PLAYERS)
            players[count++] = candidates[i];
    }
    if (count == 0) {
        snprintf(players[0].name, sizeof(players[0].name), "%s", "xdg-open");
        snprintf(players[0].exec, sizeof(players[0].exec), "%s", "xdg-open");
        count = 1;
    }
    return count;
}

static bool is_video(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    return strcasecmp(ext, ".mp4") == 0 ||
           strcasecmp(ext, ".mkv") == 0 ||
           strcasecmp(ext, ".webm") == 0 ||
           strcasecmp(ext, ".avi") == 0 ||
           strcasecmp(ext, ".mov") == 0 ||
           strcasecmp(ext, ".m4v") == 0 ||
           strcasecmp(ext, ".flv") == 0 ||
           strcasecmp(ext, ".wmv") == 0 ||
           strcasecmp(ext, ".ogv") == 0;
}

static void prettify_name(const char *in, char *out, size_t out_len)
{
    copy_text(out, out_len, in);
    for (char *p = out; *p; p++) {
        if (*p == '_' || *p == '.') *p = ' ';
    }
}

static void title_from_filename(const char *name, char *out, size_t out_len)
{
    copy_text(out, out_len, name);
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
    prettify_name(out, out, out_len);
}

static const char *basename_of(const char *path)
{
    const char *base = strrchr(path, '/');
    if (base && base[1]) return base + 1;
    return path;
}

static void expand_path(const char *in, char *out, size_t out_len)
{
    if (!in || in[0] == '\0') {
        out[0] = '\0';
        return;
    }
    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home)
            snprintf(out, out_len, "%s%s", home, in + 1);
        else
            copy_text(out, out_len, in);
    } else {
        copy_text(out, out_len, in);
    }
}

static int pin_find_path(const Library *lib, const char *path)
{
    for (int i = 0; i < lib->pinned_count; i++)
        if (strcmp(lib->pinned_paths[i], path) == 0) return i;
    return -1;
}

static bool is_pinned_path(const Library *lib, const char *path)
{
    return pin_find_path(lib, path) >= 0;
}

static void pin_add_path(Library *lib, const char *path)
{
    if (path[0] == '\0') return;
    if (pin_find_path(lib, path) >= 0) return;
    if (lib->pinned_count >= MAX_COLLECTIONS) return;
    copy_text(lib->pinned_paths[lib->pinned_count++], PATH_MAX, path);
}

static void pin_remove_path(Library *lib, const char *path)
{
    int idx = pin_find_path(lib, path);
    if (idx < 0) return;
    for (int i = idx; i < lib->pinned_count - 1; i++)
        copy_text(lib->pinned_paths[i], PATH_MAX, lib->pinned_paths[i + 1]);
    if (lib->pinned_count > 0) lib->pinned_count--;
}

static int collection_find_by_path(const Library *lib, const char *path)
{
    for (int i = 0; i < lib->collection_count; i++)
        if (strcmp(lib->collections[i].path, path) == 0) return i;
    return -1;
}

static int collection_add(Library *lib, const char *name, const char *path)
{
    if (lib->collection_count >= MAX_COLLECTIONS) return -1;
    Collection *col = &lib->collections[lib->collection_count++];
    memset(col, 0, sizeof(*col));
    copy_text(col->name, sizeof(col->name), name);
    copy_text(col->path, sizeof(col->path), path);
    col->first_media_idx = -1;
    col->pinned = is_pinned_path(lib, path);
    return lib->collection_count - 1;
}

static int collection_get_or_add(Library *lib, const char *name, const char *path)
{
    int idx = collection_find_by_path(lib, path);
    if (idx >= 0) return idx;
    return collection_add(lib, name, path);
}

static void collection_add_media(Collection *col, int media_idx)
{
    if (col->media_count >= MAX_ITEMS) return;
    col->media_indices[col->media_count++] = media_idx;
    if (col->first_media_idx < 0) col->first_media_idx = media_idx;
}

static bool media_exists(const Library *lib, const char *path)
{
    for (int i = 0; i < lib->item_count; i++)
        if (strcmp(lib->items[i].path, path) == 0) return true;
    return false;
}

static void add_media_item(Library *lib, const char *path, const char *name,
                           const char *section, int collection_idx)
{
    if (lib->item_count >= MAX_ITEMS) return;
    if (media_exists(lib, path)) return;

    MediaItem *it = &lib->items[lib->item_count];
    copy_text(it->path, sizeof(it->path), path);
    title_from_filename(name, it->title, sizeof(it->title));
    copy_text(it->section, sizeof(it->section), section);
    it->collection_idx = collection_idx;
    it->episode_number = 0;
    it->has_progress = false;
    it->progress_sec = 0.0;
    it->watched = false;

    if (collection_idx >= 0 && collection_idx < lib->collection_count)
        collection_add_media(&lib->collections[collection_idx], lib->item_count);

    lib->item_count++;
}

static MediaItem *find_media_by_path(Library *lib, const char *path)
{
    for (int i = 0; i < lib->item_count; i++) {
        if (strcmp(lib->items[i].path, path) == 0)
            return &lib->items[i];
    }
    return NULL;
}

static void trim_line(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
}

static void load_watch_progress(Library *lib)
{
    char watch_dir[PATH_MAX];
    ensure_watch_later_dir(watch_dir, sizeof(watch_dir));

    DIR *dir = opendir(watch_dir);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char full[PATH_MAX];
        if (!path_join(full, sizeof(full), watch_dir, entry->d_name))
            continue;

        FILE *fp = fopen(full, "r");
        if (!fp) continue;

        char line[PATH_MAX + 64];
        char target_path[PATH_MAX] = "";
        double progress = -1.0;

        while (fgets(line, sizeof(line), fp)) {
            trim_line(line);
            if (strncmp(line, "# ", 2) == 0 && target_path[0] == '\0') {
                copy_text(target_path, sizeof(target_path), line + 2);
            } else if (strncmp(line, "start=", 6) == 0) {
                progress = atof(line + 6);
            }
        }
        fclose(fp);

        if (target_path[0] == '\0' || progress <= 0.0)
            continue;

        MediaItem *it = find_media_by_path(lib, target_path);
        if (!it) continue;
        it->has_progress = true;
        it->progress_sec = progress;
    }

    closedir(dir);
}

static void load_watched_items(Library *lib)
{
    char path[PATH_MAX];
    watched_file_path(path, sizeof(path));

    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[PATH_MAX + 16];
    while (fgets(line, sizeof(line), fp)) {
        trim_line(line);
        if (line[0] == '\0') continue;
        MediaItem *it = find_media_by_path(lib, line);
        if (it) it->watched = true;
    }

    fclose(fp);
}

static void save_watched_items(const Library *lib)
{
    char path[PATH_MAX];
    watched_file_path(path, sizeof(path));

    FILE *fp = fopen(path, "w");
    if (!fp) return;

    for (int i = 0; i < lib->item_count; i++) {
        if (lib->items[i].watched)
            fprintf(fp, "%s\n", lib->items[i].path);
    }

    fclose(fp);
}

static void scan_source(Library *lib, const char *dir_path,
                        const char *base_path, const char *base_label, int depth)
{
    if (depth > 12) return;
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char full[PATH_MAX];
        if (!path_join(full, sizeof(full), dir_path, entry->d_name))
            continue;

        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_source(lib, full, base_path, base_label, depth + 1);
            continue;
        }

        if (!S_ISREG(st.st_mode) || !is_video(entry->d_name))
            continue;

        const char *rel = full;
        size_t base_len = strlen(base_path);
        if (strncmp(full, base_path, base_len) == 0) {
            rel = full + base_len;
            while (*rel == '/') rel++;
        }

        const char *slash = strchr(rel, '/');
        if (!slash) {
            add_media_item(lib, full, entry->d_name, base_label, -1);
            continue;
        }

        char first_part[NAME_MAX];
        size_t part_len = (size_t)(slash - rel);
        if (part_len >= sizeof(first_part)) part_len = sizeof(first_part) - 1;
        memcpy(first_part, rel, part_len);
        first_part[part_len] = '\0';

        char collection_name[SECTION_LEN];
        prettify_name(first_part, collection_name, sizeof(collection_name));

        char collection_path[PATH_MAX];
        if (!path_join(collection_path, sizeof(collection_path), base_path, first_part))
            continue;

        int collection_idx = collection_get_or_add(lib, collection_name, collection_path);
        add_media_item(lib, full, entry->d_name, collection_name, collection_idx);
    }

    closedir(dir);
}

static void library_free_thumbs(Library *lib)
{
    if (!lib) return;
    for (int i = 0; i < MAX_ITEMS; i++) {
        if (lib->thumbs[i]) {
            SDL_DestroyTexture(lib->thumbs[i]);
            lib->thumbs[i] = NULL;
        }
    }
    lib->thumbs_loaded = false;
    lib->thumb_cursor = 0;
}

static void library_destroy(Library *lib)
{
    if (!lib) return;
    library_free_thumbs(lib);
    free(lib);
}

static void ui_close(UI *ui)
{
    if (!ui) return;
    if (ui->sm) {
        TTF_CloseFont(ui->sm);
        ui->sm = NULL;
    }
    if (ui->md) {
        TTF_CloseFont(ui->md);
        ui->md = NULL;
    }
    if (ui->lg) {
        TTF_CloseFont(ui->lg);
        ui->lg = NULL;
    }
}

static const char *view_title(const Library *lib, const ViewItem *view)
{
    if (view->kind == VIEW_COLLECTION)
        return lib->collections[view->index].name;
    return lib->items[view->index].title;
}

static int cmp_media_index(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    const MediaItem *ma = &sort_lib->items[ia];
    const MediaItem *mb = &sort_lib->items[ib];

    int cmp = natural_compare(ma->title, mb->title);
    if (cmp != 0) return cmp;
    return strcasecmp(ma->path, mb->path);
}

static int cmp_view_item(const void *a, const void *b)
{
    const ViewItem *va = (const ViewItem *)a;
    const ViewItem *vb = (const ViewItem *)b;
    int sec = 0;
    if (strcmp(va->section, "Folders") == 0 && strcmp(vb->section, "Folders") != 0)
        sec = -1;
    else if (strcmp(va->section, "Folders") != 0 && strcmp(vb->section, "Folders") == 0)
        sec = 1;
    else
        sec = strcasecmp(va->section, vb->section);
    if (sec != 0) return sec;

    if (va->kind != vb->kind)
        return va->kind == VIEW_COLLECTION ? -1 : 1;

    return strcasecmp(view_title(sort_lib, va), view_title(sort_lib, vb));
}

static void rebuild_sections(Library *lib)
{
    lib->section_count = 0;
    if (lib->view_count <= 0) return;

    for (int i = 0; i < lib->view_count; i++) {
        if (lib->section_count > 0 &&
            strcmp(lib->sections[lib->section_count - 1].name, lib->view_items[i].section) == 0) {
            lib->sections[lib->section_count - 1].count++;
            continue;
        }
        if (lib->section_count >= MAX_SECTIONS) break;
        Section *sec = &lib->sections[lib->section_count++];
        memset(sec, 0, sizeof(*sec));
        copy_text(sec->name, sizeof(sec->name), lib->view_items[i].section);
        sec->first = i;
        sec->count = 1;
    }
}

static void select_first_visible(Library *lib);

static void sort_collection_media(Library *lib)
{
    sort_lib = lib;
    for (int i = 0; i < lib->collection_count; i++) {
        Collection *col = &lib->collections[i];
        if (col->media_count > 1)
            qsort(col->media_indices, (size_t)col->media_count, sizeof(int), cmp_media_index);
        col->first_media_idx = col->media_count > 0 ? col->media_indices[0] : -1;
        for (int j = 0; j < col->media_count; j++)
            lib->items[col->media_indices[j]].episode_number = j + 1;
    }
}

static void rebuild_view(Library *lib)
{
    lib->view_count = 0;

    if (lib->in_collection_view) {
        int collection_idx = collection_find_by_path(lib, lib->active_collection_path);
        if (collection_idx < 0) {
            lib->in_collection_view = false;
            lib->active_collection_path[0] = '\0';
        } else {
            Collection *col = &lib->collections[collection_idx];
            for (int i = 0; i < col->media_count && lib->view_count < MAX_VIEW_ITEMS; i++) {
                ViewItem *view = &lib->view_items[lib->view_count++];
                view->kind = VIEW_VIDEO;
                view->index = col->media_indices[i];
                copy_text(view->section, sizeof(view->section), col->name);
            }
        }
    }

    if (!lib->in_collection_view) {
        for (int i = 0; i < lib->collection_count && lib->view_count < MAX_VIEW_ITEMS; i++) {
            ViewItem *view = &lib->view_items[lib->view_count++];
            view->kind = VIEW_COLLECTION;
            view->index = i;
            copy_text(view->section, sizeof(view->section), "Folders");
        }

        for (int i = 0; i < lib->item_count && lib->view_count < MAX_VIEW_ITEMS; i++) {
            if (lib->items[i].collection_idx >= 0)
                continue;
            ViewItem *view = &lib->view_items[lib->view_count++];
            view->kind = VIEW_VIDEO;
            view->index = i;
            copy_text(view->section, sizeof(view->section), lib->items[i].section);
        }

        for (int i = 0; i < lib->collection_count; i++) {
            Collection *col = &lib->collections[i];
            if (!col->pinned) continue;
            for (int j = 0; j < col->media_count && lib->view_count < MAX_VIEW_ITEMS; j++) {
                ViewItem *view = &lib->view_items[lib->view_count++];
                view->kind = VIEW_VIDEO;
                view->index = col->media_indices[j];
                copy_text(view->section, sizeof(view->section), col->name);
            }
        }
    }

    sort_lib = lib;
    qsort(lib->view_items, (size_t)lib->view_count, sizeof(ViewItem), cmp_view_item);
    rebuild_sections(lib);

    lib->max_scroll = 0;
    if (lib->selected >= lib->view_count)
        lib->selected = -1;
    select_first_visible(lib);
}

static void library_scan(Library *lib)
{
    library_free_thumbs(lib);

    lib->item_count = 0;
    lib->collection_count = 0;
    lib->selected = -1;
    lib->scroll_y = 0;
    lib->max_scroll = 0;

    mkdir(lib->root, 0755);
    scan_source(lib, lib->root, lib->root, "Videos", 0);

    for (int i = 0; i < lib->extra_root_count; i++) {
        char label[SECTION_LEN];
        prettify_name(basename_of(lib->extra_roots[i]), label, sizeof(label));
        scan_source(lib, lib->extra_roots[i], lib->extra_roots[i], label, 0);
    }

    load_watch_progress(lib);
    load_watched_items(lib);
    sort_collection_media(lib);
    rebuild_view(lib);
}

static bool root_exists(const Library *lib, const char *path)
{
    if (strcmp(lib->root, path) == 0) return true;
    for (int i = 0; i < lib->extra_root_count; i++)
        if (strcmp(lib->extra_roots[i], path) == 0) return true;
    return false;
}

static bool library_add_folder(Library *lib, const char *input_path)
{
    char path[PATH_MAX];
    expand_path(input_path, path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        return false;
    if (root_exists(lib, path))
        return true;
    if (lib->extra_root_count >= MAX_EXTRA_ROOTS)
        return false;

    copy_text(lib->extra_roots[lib->extra_root_count++], PATH_MAX, path);
    library_scan(lib);
    return true;
}

static bool make_thumbnail(const Library *lib, const MediaItem *it, char *bmp_path, size_t path_len)
{
    char thumb_dir[PATH_MAX];
    ensure_thumb_dir(lib, thumb_dir, sizeof(thumb_dir));
    struct stat st;
    long mtime = stat(it->path, &st) == 0 ? (long)st.st_mtime : 0;
    char thumb_name[96];
    snprintf(thumb_name, sizeof(thumb_name), "%016lx_%ld_wide_v2.bmp",
             hash_string(it->path), mtime);
    if (!path_join(bmp_path, path_len, thumb_dir, thumb_name))
        return false;

    if (access(bmp_path, R_OK) == 0) return true;
    if (!command_exists("ffmpeg")) return false;

    pid_t pid = fork();
    if (pid == 0) {
        execlp("ffmpeg", "ffmpeg",
               "-y", "-hide_banner", "-loglevel", "error",
               "-i", it->path,
               "-frames:v", "1",
               "-vf", "thumbnail=120,scale=488:256:force_original_aspect_ratio=increase,crop=488:256",
               bmp_path,
               (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return false;

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && access(bmp_path, R_OK) == 0;
}

static void library_load_next_thumb(Library *lib, SDL_Renderer *r)
{
    if (lib->thumbs_loaded) return;

    while (lib->thumb_cursor < lib->item_count) {
        int i = lib->thumb_cursor++;
        if (lib->thumbs[i]) continue;

        char bmp_path[PATH_MAX];
        if (!make_thumbnail(lib, &lib->items[i], bmp_path, sizeof(bmp_path)))
            continue;

        SDL_Surface *surf = SDL_LoadBMP(bmp_path);
        if (!surf) continue;

        lib->thumbs[i] = SDL_CreateTextureFromSurface(r, surf);
        SDL_FreeSurface(surf);
        return;
    }

    lib->thumbs_loaded = true;
}

static SDL_Texture *view_thumb(const Library *lib, const ViewItem *view)
{
    if (view->kind == VIEW_VIDEO)
        return lib->thumbs[view->index];

    const Collection *col = &lib->collections[view->index];
    if (col->first_media_idx < 0) return NULL;
    return lib->thumbs[col->first_media_idx];
}

static const char *view_search_title(const Library *lib, const ViewItem *view)
{
    return view_title(lib, view);
}

static bool matches_search(const Library *lib, int idx)
{
    if (idx < 0 || idx >= lib->view_count) return false;
    if (lib->search[0] == '\0') return true;

    const ViewItem *view = &lib->view_items[idx];
    if (strcasestr(view_search_title(lib, view), lib->search))
        return true;
    if (strcasestr(view->section, lib->search))
        return true;

    if (view->kind == VIEW_COLLECTION) {
        const Collection *col = &lib->collections[view->index];
        return strcasestr(col->path, lib->search) != NULL;
    }

    const MediaItem *it = &lib->items[view->index];
    return strcasestr(it->path, lib->search) != NULL;
}

static int visible_next(const Library *lib, int start, int dir)
{
    if (lib->view_count <= 0) return -1;

    int idx = start;
    for (int i = 0; i < lib->view_count; i++) {
        idx += dir;
        if (idx < 0) idx = lib->view_count - 1;
        if (idx >= lib->view_count) idx = 0;
        if (matches_search(lib, idx)) return idx;
    }
    return start >= 0 ? start : -1;
}

static void select_first_visible(Library *lib)
{
    if (lib->selected >= 0 && lib->selected < lib->view_count && matches_search(lib, lib->selected))
        return;
    lib->selected = visible_next(lib, -1, 1);
}

static void draw_rect(SDL_Renderer *r, int x, int y, int w, int h,
                      Uint8 rr, Uint8 g, Uint8 b, Uint8 a)
{
    SDL_SetRenderDrawBlendMode(r, a < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, rr, g, b, a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(r, &rect);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

static void draw_text(SDL_Renderer *r, TTF_Font *f, const char *txt,
                      int x, int y, Uint8 rr, Uint8 g, Uint8 b)
{
    if (!f || !txt || txt[0] == '\0') return;
    SDL_Color col = {rr, g, b, 255};
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, txt, col);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    if (tex) {
        SDL_Rect dst = {x, y, surf->w, surf->h};
        SDL_RenderCopy(r, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surf);
}

static void draw_text_fit(SDL_Renderer *r, TTF_Font *f, const char *txt,
                          int x, int y, int max_w, Uint8 rr, Uint8 g, Uint8 b)
{
    char buf[TITLE_LEN];
    snprintf(buf, sizeof(buf), "%s", txt);
    int tw = 0, th = 0;
    while (buf[0] && f && TTF_SizeUTF8(f, buf, &tw, &th) == 0 && tw > max_w) {
        size_t len = strlen(buf);
        if (len <= 4) break;
        snprintf(buf + len - 4, 5, "%s", "...");
    }
    draw_text(r, f, buf, x, y, rr, g, b);
}

static void draw_pin_badge(SDL_Renderer *r, UI *ui, int x, int y)
{
    draw_rect(r, x, y, 52, 24, 229, 9, 20, 230);
    draw_text(r, ui->sm, "PIN", x + 12, y + 4, 250, 250, 250);
}

static void format_clock(double seconds, char *out, size_t out_len)
{
    int total = seconds > 0.0 ? (int)(seconds + 0.5) : 0;
    int hh = total / 3600;
    int mm = (total % 3600) / 60;
    int ss = total % 60;
    if (hh > 0)
        snprintf(out, out_len, "%d:%02d:%02d", hh, mm, ss);
    else
        snprintf(out, out_len, "%02d:%02d", mm, ss);
}

static int collection_in_progress_count(const Library *lib, const Collection *col)
{
    int count = 0;
    for (int i = 0; i < col->media_count; i++) {
        const MediaItem *it = &lib->items[col->media_indices[i]];
        if (it->has_progress) count++;
    }
    return count;
}

static int collection_watched_count(const Library *lib, const Collection *col)
{
    int count = 0;
    for (int i = 0; i < col->media_count; i++) {
        const MediaItem *it = &lib->items[col->media_indices[i]];
        if (it->watched) count++;
    }
    return count;
}

static void draw_card(SDL_Renderer *r, UI *ui, const Library *lib,
                      const ViewItem *view, int x, int y, bool selected)
{
    Uint8 br = selected ? 70 : 28;
    Uint8 bg = selected ? 74 : 31;
    Uint8 bb = selected ? 86 : 40;
    draw_rect(r, x, y, CARD_W, CARD_H, br, bg, bb, 255);

    draw_rect(r, x + 10, y + 10, CARD_W - 20, THUMB_H, 30, 32, 40, 255);
    SDL_Texture *thumb = view_thumb(lib, view);
    if (thumb) {
        SDL_Rect dst = {x + 10, y + 10, CARD_W - 20, THUMB_H};
        SDL_RenderCopy(r, thumb, NULL, &dst);
        draw_rect(r, x + 10, y + THUMB_H - 24, CARD_W - 20, 34, 4, 6, 10, 90);
    } else {
        unsigned hash = 2166136261u;
        for (const char *p = view_title(lib, view); *p; p++)
            hash = (hash ^ (unsigned char)*p) * 16777619u;
        Uint8 ar = 38 + (hash & 63);
        Uint8 ag = 44 + ((hash >> 8) & 47);
        Uint8 ab = 58 + ((hash >> 16) & 63);
        draw_rect(r, x + 10, y + 10, CARD_W - 20, THUMB_H, ar, ag, ab, 255);
        draw_rect(r, x + 10, y + THUMB_H - 24, CARD_W - 20, 34, 8, 10, 14, 95);
    }

    if (view->kind == VIEW_COLLECTION) {
        const Collection *col = &lib->collections[view->index];
        char subtitle[TITLE_LEN];
        int in_progress = collection_in_progress_count(lib, col);
        int watched = collection_watched_count(lib, col);
        snprintf(subtitle, sizeof(subtitle), "%d %s%s%s%s",
                 col->media_count,
                 col->media_count == 1 ? "video" : "videos",
                 watched > 0 ? " | watched" : "",
                 in_progress > 0 ? " | resume" : "",
                 col->pinned ? " | pinned" : "");

        draw_text_fit(r, ui->md, col->name, x + 12, y + THUMB_H + 22, CARD_W - 24, 236, 236, 238);
        draw_text_fit(r, ui->sm, subtitle, x + 12, y + THUMB_H + 48, CARD_W - 24, 160, 164, 172);
        if (col->pinned)
            draw_pin_badge(r, ui, x + CARD_W - 66, y + 18);
    } else {
        const MediaItem *it = &lib->items[view->index];
        char episode_line[64];
        if (lib->in_collection_view && it->episode_number > 0)
            snprintf(episode_line, sizeof(episode_line), "Episode %d", it->episode_number);
        else
            snprintf(episode_line, sizeof(episode_line), "%s", it->section);

        draw_text_fit(r, ui->lg, it->title, x + 12, y + THUMB_H + 14, CARD_W - 24, 236, 236, 238);
        if (it->watched) {
            char subtitle[TITLE_LEN];
            if (it->has_progress) {
                char progress[64];
                format_clock(it->progress_sec, progress, sizeof(progress));
                snprintf(subtitle, sizeof(subtitle), "%s | watched | resume %s", episode_line, progress);
            } else {
                snprintf(subtitle, sizeof(subtitle), "%s | watched", episode_line);
            }
            draw_text_fit(r, ui->sm, subtitle, x + 12, y + THUMB_H + 56, CARD_W - 24, 160, 164, 172);
            draw_rect(r, x + CARD_W - 84, y + 18, 70, 24, 44, 150, 86, 235);
            draw_text(r, ui->sm, "WATCHED", x + CARD_W - 78, y + 22, 246, 250, 248);
            draw_rect(r, x + 10, y + 10, CARD_W - 20, 5, 44, 150, 86, 220);
        } else if (it->has_progress) {
            char progress[64];
            char subtitle[TITLE_LEN];
            format_clock(it->progress_sec, progress, sizeof(progress));
            snprintf(subtitle, sizeof(subtitle), "%s | resume %s", episode_line, progress);
            draw_text_fit(r, ui->sm, subtitle, x + 12, y + THUMB_H + 56, CARD_W - 24, 160, 164, 172);
            draw_rect(r, x + 10, y + 10, CARD_W - 20, 5, 229, 9, 20, 220);
        } else {
            draw_text_fit(r, ui->sm, episode_line, x + 12, y + THUMB_H + 56, CARD_W - 24, 160, 164, 172);
        }
    }

    if (selected) {
        SDL_SetRenderDrawColor(r, 229, 9, 20, 255);
        SDL_Rect border = {x - 3, y - 3, CARD_W + 6, CARD_H + 6};
        SDL_RenderDrawRect(r, &border);
    }
}

static int layout_library(Library *lib, int *card_x, int *card_y, bool write_positions)
{
    int y = TOP_H + 28;
    int content_bottom = y;
    int cols = (win_w - LEFT_PAD * 2 + CARD_GAP) / (CARD_W + CARD_GAP);
    if (cols < 1) cols = 1;

    for (int s = 0; s < lib->section_count; s++) {
        Section *sec = &lib->sections[s];
        int row_items = 0;
        int header_y = y;
        y += 42;
        sec->y = header_y;

        for (int idx = 0; idx < lib->view_count; idx++) {
            if (strcmp(lib->view_items[idx].section, sec->name) != 0) continue;
            if (!matches_search(lib, idx)) continue;

            int col = row_items % cols;
            int row = row_items / cols;
            if (write_positions) {
                card_x[idx] = LEFT_PAD + col * (CARD_W + CARD_GAP);
                card_y[idx] = y + row * (CARD_H + CARD_GAP);
            }
            row_items++;
        }

        if (row_items > 0) {
            int rows = (row_items + cols - 1) / cols;
            y += rows * (CARD_H + CARD_GAP) + ROW_GAP;
            content_bottom = y;
        } else if (lib->search[0] == '\0') {
            y += 42 + ROW_GAP;
            content_bottom = y;
        } else {
            y = header_y;
        }
    }

    lib->max_scroll = content_bottom - win_h + 20;
    if (lib->max_scroll < 0) lib->max_scroll = 0;
    if (lib->scroll_y > lib->max_scroll) lib->scroll_y = lib->max_scroll;
    return content_bottom;
}

static void render(SDL_Renderer *r, UI *ui, Library *lib,
                   const Player *players, int player_idx)
{
    draw_rect(r, 0, 0, win_w, win_h, 13, 14, 18, 255);
    draw_rect(r, 0, 0, win_w, TOP_H, 10, 10, 13, 245);
    draw_rect(r, 0, TOP_H - 1, win_w, 1, 229, 9, 20, 180);
    draw_text(r, ui->lg, "cwatch", 28, 22, 229, 9, 20);

    char info[768];
    if (lib->in_collection_view) {
        char folder[SECTION_LEN];
        prettify_name(basename_of(lib->active_collection_path), folder, sizeof(folder));
        snprintf(info, sizeof(info),
                 "folder: %s  |  player: %s  |  Enter open  i pin  m watched  Backspace back  p player  / search  r rescan",
                 folder, players[player_idx].name);
    } else {
        char root_label[SECTION_LEN];
        copy_text(root_label, sizeof(root_label), lib->root);
        snprintf(info, sizeof(info),
                 "library: %s  |  player: %s  |  Enter open/folder  i pin folder  m watched video  RightClick pin  d add folder  / search  p player  r rescan",
                 root_label, players[player_idx].name);
    }
    draw_text_fit(r, ui->sm, info, 168, 34, win_w - 200, 176, 178, 184);

    int card_x[MAX_VIEW_ITEMS], card_y[MAX_VIEW_ITEMS];
    for (int i = 0; i < MAX_VIEW_ITEMS; i++) card_x[i] = card_y[i] = -10000;
    layout_library(lib, card_x, card_y, true);

    SDL_Rect clip = {0, TOP_H, win_w, win_h - TOP_H};
    SDL_RenderSetClipRect(r, &clip);

    for (int s = 0; s < lib->section_count; s++) {
        Section *sec = &lib->sections[s];
        bool any = false;
        for (int idx = 0; idx < lib->view_count; idx++) {
            if (strcmp(lib->view_items[idx].section, sec->name) != 0) continue;
            if (matches_search(lib, idx)) {
                any = true;
                break;
            }
        }

        if (!any && lib->search[0] != '\0') continue;

        int sy = sec->y - lib->scroll_y;
        if (sy > TOP_H - 40 && sy < win_h)
            draw_text(r, ui->lg, sec->name, LEFT_PAD, sy, 245, 245, 247);

        if (!any) {
            draw_text(r, ui->sm, "No items in this section yet",
                      LEFT_PAD, sy + 46, 126, 130, 138);
            continue;
        }

        for (int idx = 0; idx < lib->view_count; idx++) {
            if (strcmp(lib->view_items[idx].section, sec->name) != 0) continue;
            if (!matches_search(lib, idx)) continue;

            int x = card_x[idx];
            int y = card_y[idx] - lib->scroll_y;
            if (y + CARD_H < TOP_H || y > win_h) continue;
            draw_card(r, ui, lib, &lib->view_items[idx], x, y, lib->selected == idx);
        }
    }

    SDL_RenderSetClipRect(r, NULL);

    if (lib->view_count == 0) {
        const char *msg = lib->in_collection_view
            ? "No videos in this folder."
            : "No videos found. Put media inside ./media or run ./cwatch /path/to/media.";
        draw_text(r, ui->md, msg, LEFT_PAD, TOP_H + 48, 176, 178, 184);
    }

    if (lib->search_active) {
        int w = 520, h = 46;
        int x = (win_w - w) / 2;
        int y = win_h - h - 24;
        draw_rect(r, x - 4, y - 4, w + 8, h + 8, 0, 0, 0, 170);
        draw_rect(r, x, y, w, h, 28, 30, 38, 255);
        char q[320];
        snprintf(q, sizeof(q), "Search: %s|", lib->search);
        draw_text_fit(r, ui->md, q, x + 14, y + 12, w - 28, 238, 238, 240);
    } else if (lib->input_active) {
        int w = 640, h = 46;
        int x = (win_w - w) / 2;
        int y = win_h - h - 24;
        draw_rect(r, x - 4, y - 4, w + 8, h + 8, 0, 0, 0, 170);
        draw_rect(r, x, y, w, h, 28, 30, 38, 255);
        char q[PATH_MAX + 32];
        snprintf(q, sizeof(q), "Add folder: %s|", lib->input_path);
        draw_text_fit(r, ui->md, q, x + 14, y + 12, w - 28, 238, 238, 240);
    }

    if (lib->max_scroll > 0) {
        int track_h = win_h - TOP_H;
        int bar_h = track_h * track_h / (track_h + lib->max_scroll);
        if (bar_h < 40) bar_h = 40;
        int bar_y = TOP_H + (track_h - bar_h) * lib->scroll_y / lib->max_scroll;
        draw_rect(r, win_w - 6, bar_y, 4, bar_h, 229, 9, 20, 170);
    }
}

static void open_item(const MediaItem *it, const Player *player)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (strcmp(player->exec, "mpv") == 0) {
            char watch_dir[PATH_MAX];
            char watch_dir_arg[PATH_MAX + 32];
            ensure_watch_later_dir(watch_dir, sizeof(watch_dir));
            snprintf(watch_dir_arg, sizeof(watch_dir_arg),
                     "--watch-later-directory=%s", watch_dir);
            execlp(player->exec, player->exec,
                   "--save-position-on-quit=yes",
                   "--resume-playback=yes",
                   "--write-filename-in-watch-later-config=yes",
                   "--watch-later-options=start",
                   watch_dir_arg,
                   it->path,
                   (char *)NULL);
        }
        execlp(player->exec, player->exec, it->path, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        int status;
        waitpid(pid, &status, WNOHANG);
    }
}

static void enter_collection(Library *lib, int collection_idx)
{
    if (collection_idx < 0 || collection_idx >= lib->collection_count) return;
    lib->in_collection_view = true;
    copy_text(lib->active_collection_path, sizeof(lib->active_collection_path),
              lib->collections[collection_idx].path);
    lib->selected = -1;
    lib->scroll_y = 0;
    rebuild_view(lib);
}

static void leave_collection(Library *lib)
{
    lib->in_collection_view = false;
    lib->active_collection_path[0] = '\0';
    lib->selected = -1;
    lib->scroll_y = 0;
    rebuild_view(lib);
}

static void toggle_collection_pin(Library *lib, int collection_idx)
{
    if (collection_idx < 0 || collection_idx >= lib->collection_count) return;
    Collection *col = &lib->collections[collection_idx];
    col->pinned = !col->pinned;
    if (col->pinned)
        pin_add_path(lib, col->path);
    else
        pin_remove_path(lib, col->path);
    rebuild_view(lib);
}

static void toggle_selected_pin(Library *lib)
{
    if (lib->in_collection_view) {
        int collection_idx = collection_find_by_path(lib, lib->active_collection_path);
        if (collection_idx >= 0)
            toggle_collection_pin(lib, collection_idx);
        return;
    }

    if (lib->selected < 0 || lib->selected >= lib->view_count) return;
    ViewItem *view = &lib->view_items[lib->selected];
    if (view->kind == VIEW_COLLECTION)
        toggle_collection_pin(lib, view->index);
}

static void toggle_selected_watched(Library *lib)
{
    if (lib->selected < 0 || lib->selected >= lib->view_count) return;

    ViewItem *view = &lib->view_items[lib->selected];
    if (view->kind != VIEW_VIDEO) return;

    MediaItem *it = &lib->items[view->index];
    it->watched = !it->watched;
    save_watched_items(lib);
}

static void activate_view_item(Library *lib, const Player *player, int idx)
{
    if (idx < 0 || idx >= lib->view_count) return;

    ViewItem *view = &lib->view_items[idx];
    if (view->kind == VIEW_COLLECTION) {
        enter_collection(lib, view->index);
        return;
    }

    open_item(&lib->items[view->index], player);
}

static void ensure_selected_visible(Library *lib)
{
    int card_x[MAX_VIEW_ITEMS], card_y[MAX_VIEW_ITEMS];
    for (int i = 0; i < MAX_VIEW_ITEMS; i++) card_x[i] = card_y[i] = -10000;
    layout_library(lib, card_x, card_y, true);
    if (lib->selected < 0 || lib->selected >= lib->view_count) return;
    int y = card_y[lib->selected];
    if (y < -9000) return;
    if (y - lib->scroll_y < TOP_H + 12) lib->scroll_y = y - TOP_H - 12;
    if (y + CARD_H - lib->scroll_y > win_h - 12) lib->scroll_y = y + CARD_H - win_h + 12;
    if (lib->scroll_y < 0) lib->scroll_y = 0;
    if (lib->scroll_y > lib->max_scroll) lib->scroll_y = lib->max_scroll;
}

static int hit_test(Library *lib, int mx, int my)
{
    int card_x[MAX_VIEW_ITEMS], card_y[MAX_VIEW_ITEMS];
    for (int i = 0; i < MAX_VIEW_ITEMS; i++) card_x[i] = card_y[i] = -10000;
    layout_library(lib, card_x, card_y, true);
    for (int i = 0; i < lib->view_count; i++) {
        if (!matches_search(lib, i)) continue;
        int x = card_x[i];
        int y = card_y[i] - lib->scroll_y;
        if (mx >= x && mx <= x + CARD_W && my >= y && my <= y + CARD_H)
            return i;
    }
    return -1;
}

static void init_default_root(char *out, size_t out_len, const char *argv0)
{
    (void)argv0;
    const char *home = getenv("HOME");
    if (home && home[0] != '\0')
        path_join(out, out_len, home, "media");
    else
        copy_text(out, out_len, "media");
}

int main(int argc, char **argv)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(APP_TITLE, SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        TTF_Quit();
        SDL_Quit();
        return 1;
    }
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    UI ui = {load_font(13), load_font(16), load_font(25)};
    Player players[MAX_PLAYERS];
    int player_count = discover_players(players);
    int player_idx = 0;

    Library *lib = calloc(1, sizeof(*lib));
    if (!lib) {
        fprintf(stderr, "Out of memory\n");
        ui_close(&ui);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    if (argc >= 2) copy_text(lib->root, sizeof(lib->root), argv[1]);
    else init_default_root(lib->root, sizeof(lib->root), argv[0]);

    library_scan(lib);
    SDL_StartTextInput();

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = false;
            } else if (ev.type == SDL_WINDOWEVENT &&
                       ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                win_w = ev.window.data1;
                win_h = ev.window.data2;
            } else if (ev.type == SDL_MOUSEWHEEL) {
                lib->scroll_y -= ev.wheel.y * 72;
                if (lib->scroll_y < 0) lib->scroll_y = 0;
                if (lib->scroll_y > lib->max_scroll) lib->scroll_y = lib->max_scroll;
            } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
                int idx = hit_test(lib, ev.button.x, ev.button.y);
                if (idx >= 0) {
                    lib->selected = idx;
                    activate_view_item(lib, &players[player_idx], idx);
                }
            } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_RIGHT) {
                int idx = hit_test(lib, ev.button.x, ev.button.y);
                if (idx >= 0) {
                    lib->selected = idx;
                    if (lib->view_items[idx].kind == VIEW_COLLECTION)
                        toggle_collection_pin(lib, lib->view_items[idx].index);
                }
            } else if (ev.type == SDL_TEXTINPUT && (lib->search_active || lib->input_active)) {
                if (lib->ignore_next_text) {
                    lib->ignore_next_text = false;
                } else if (lib->search_active) {
                    size_t len = strlen(lib->search);
                    if (len + strlen(ev.text.text) < sizeof(lib->search))
                        strcat(lib->search, ev.text.text);
                    select_first_visible(lib);
                } else {
                    size_t len = strlen(lib->input_path);
                    if (len + strlen(ev.text.text) < sizeof(lib->input_path))
                        strcat(lib->input_path, ev.text.text);
                }
            } else if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode key = ev.key.keysym.sym;
                if (lib->input_active) {
                    if (key == SDLK_ESCAPE) {
                        lib->input_active = false;
                        lib->input_path[0] = '\0';
                    } else if (key == SDLK_BACKSPACE) {
                        size_t len = strlen(lib->input_path);
                        if (len > 0) lib->input_path[len - 1] = '\0';
                    } else if (key == SDLK_RETURN) {
                        if (lib->input_path[0] != '\0')
                            library_add_folder(lib, lib->input_path);
                        lib->input_active = false;
                        lib->input_path[0] = '\0';
                        ensure_selected_visible(lib);
                    }
                } else if (lib->search_active) {
                    if (key == SDLK_ESCAPE) {
                        lib->search_active = false;
                        lib->search[0] = '\0';
                        select_first_visible(lib);
                    } else if (key == SDLK_BACKSPACE) {
                        size_t len = strlen(lib->search);
                        if (len > 0) lib->search[len - 1] = '\0';
                        select_first_visible(lib);
                    } else if (key == SDLK_RETURN) {
                        lib->search_active = false;
                    }
                } else if ((key == SDLK_ESCAPE || key == SDLK_BACKSPACE) && lib->in_collection_view) {
                    leave_collection(lib);
                } else if (key == SDLK_ESCAPE) {
                    running = false;
                } else if (key == SDLK_SLASH) {
                    lib->search_active = true;
                    lib->search[0] = '\0';
                    lib->ignore_next_text = true;
                } else if (key == SDLK_d) {
                    lib->input_active = true;
                    lib->input_path[0] = '\0';
                    lib->ignore_next_text = true;
                } else if (key == SDLK_r) {
                    library_scan(lib);
                } else if (key == SDLK_p) {
                    player_idx = (player_idx + 1) % player_count;
                } else if (key == SDLK_i) {
                    toggle_selected_pin(lib);
                } else if (key == SDLK_m) {
                    toggle_selected_watched(lib);
                } else if (key == SDLK_RETURN || key == SDLK_SPACE) {
                    if (lib->selected >= 0)
                        activate_view_item(lib, &players[player_idx], lib->selected);
                } else if (key == SDLK_RIGHT) {
                    lib->selected = visible_next(lib, lib->selected, 1);
                    ensure_selected_visible(lib);
                } else if (key == SDLK_LEFT) {
                    lib->selected = visible_next(lib, lib->selected, -1);
                    ensure_selected_visible(lib);
                } else if (key == SDLK_DOWN) {
                    for (int i = 0; i < 4; i++) lib->selected = visible_next(lib, lib->selected, 1);
                    ensure_selected_visible(lib);
                } else if (key == SDLK_UP) {
                    for (int i = 0; i < 4; i++) lib->selected = visible_next(lib, lib->selected, -1);
                    ensure_selected_visible(lib);
                } else if (key == SDLK_PAGEDOWN) {
                    lib->scroll_y += win_h - TOP_H;
                    if (lib->scroll_y > lib->max_scroll) lib->scroll_y = lib->max_scroll;
                } else if (key == SDLK_PAGEUP) {
                    lib->scroll_y -= win_h - TOP_H;
                    if (lib->scroll_y < 0) lib->scroll_y = 0;
                }
            }
        }

        render(renderer, &ui, lib, players, player_idx);
        SDL_RenderPresent(renderer);
        if (!lib->thumbs_loaded)
            library_load_next_thumb(lib, renderer);
    }

    SDL_StopTextInput();

    ui_close(&ui);
    library_destroy(lib);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
