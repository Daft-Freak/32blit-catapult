#include <cstring>
#include <list>
#include <optional>

#include "32blit.hpp"
#include "engine/api_private.hpp"
#include "executable.hpp"
#include "metadata.hpp"

#include "assets.hpp"

using namespace blit;

const Size splash_size(128, 96);
const Point splash_half_size(splash_size.w / 2, splash_size.h / 2);

const int file_item_width = splash_size.w + 2;

struct PathSave {
  char last_path[512];
};

static const int path_save_slot = 256;

struct MetadataCacheEntry {
    BlitGameMetadata data;
    bool valid = false;
    bool can_launch;
    std::string path;
};

std::list<MetadataCacheEntry> metadata_cache;

static std::string path = "";
static std::string dir_change_old_path; // if we just entered/exited a dir

// TODO: custom struct if we need more info
static std::vector<FileInfo> file_list;
static int file_list_offset = 0;

static Point scroll_offset;

static Surface *default_splash, *folder_splash;

static const Font launcher_font(asset_font8x8);

// startup animation
const int startup_fade_len = 75;
static int startup_fade = startup_fade_len;

// launch animation
const int launch_anim_len = 30;
static int launch_anim_time = launch_anim_len;
static bool do_launch = false;

static std::string join_path(const std::string &a, const std::string &b) {
    if(a.empty())
        return b;

    std::string ret;
    ret.reserve(a.length() + b.length() + 1);

    ret = a;

    if(ret[ret.length() - 1] != '/')
        ret += '/';
    
    ret += b;

    return ret;
}

// splits the last part of the path
// e.g. /path/to/thing -> /path/to, thing
static std::pair<std::string_view, std::string_view> split_path_last(const std::string_view path) {
    auto pos = path.find_last_of('/');

    // /thing, first part is /
    if(pos == 0)
        return {path.substr(0, 1), path.substr(1)};

    auto len = path.length();

    // trailing slash
    if(pos == path.length() - 1) {
        pos = path.find_last_of('/', pos - 1);
        len--;
    }

    return {path.substr(0, pos), path.substr(pos + 1, len - pos - 1)};
}

static bool should_display_file(const std::string &path) {
    if(!api.can_launch)
        return true;

    auto res = api.can_launch(path.c_str());

    // display launchable and incompatible
    return res == CanLaunchResult::Success || res == CanLaunchResult::IncompatibleBlit;
}

static void update_file_list() {
    if(path == "") {
        // top level installed/storage selection

        file_list.clear();
        file_list.emplace_back(FileInfo{"/", FileFlags::directory, 0});

        if(api.list_installed_games)
            file_list.emplace_back(FileInfo{"flash:", FileFlags::directory, 0});
    } else {
        file_list = list_files(path, [](const FileInfo &info){
            // hidden file
            if(info.name[0] == '.' || info.name == "System Volume Information")
                return false;

            if(info.flags & FileFlags::directory)
                return true;

            return should_display_file(join_path(path, info.name));
        });
    }

    // TODO: use a smaller sort function? (like the SDK launcher)
    std::sort(file_list.begin(), file_list.end(), [](const auto &a, const auto &b) {return a.name < b.name;});

    file_list_offset = 0;
    scroll_offset.x = 0;
}

static void scroll_list_to(const std::string_view filename) {
    int offset = 0;
    for(auto & info : file_list) {
        if(info.name == filename)
            break;

        offset++;
    }

    // didn't find it
    if(offset == int(file_list.size()))
        return;

    file_list_offset = offset;
    scroll_offset.x = offset * file_item_width;
}

// TODO: this is copied from the SDK launcher...
static bool parse_file_metadata(const std::string &filename, BlitGameMetadata &metadata, bool unpack_images = false) {
    blit::File f(filename);

    if(!f.is_open())
        return false;

    uint32_t offset = 0;

    uint8_t buf[sizeof(BlitGameHeader)];
    auto read = f.read(offset, sizeof(buf), (char *)&buf);

    // skip relocation data
    if(memcmp(buf, "RELO", 4) == 0) {
        uint32_t num_relocs;
        f.read(4, 4, (char *)&num_relocs);

        offset = num_relocs * 4 + 8;
        // re-read header
        read = f.read(offset, sizeof(buf), (char *)&buf);
    }

    // game header - skip to metadata
    if(memcmp(buf, "BLITMETA", 8) != 0) {
        auto &header = *(BlitGameHeader *)buf;
        if(read == sizeof(BlitGameHeader) && header.magic == blit_game_magic) {
            offset += (header.end & 0x1FFFFFF);
            read = f.read(offset, 10, (char *)buf);
        }
    }

    if(read >= 10 && memcmp(buf, "BLITMETA", 8) == 0) {
        // don't bother reading the whole thing if we don't want the images
        auto metadata_len = unpack_images ? *reinterpret_cast<uint16_t *>(buf + 8) : sizeof(RawMetadata);

        uint8_t metadata_buf[0xFFFF];
        f.read(offset + 10, metadata_len, (char *)metadata_buf);

        parse_metadata(reinterpret_cast<char *>(metadata_buf), metadata_len, metadata, unpack_images);

        return true;
    }

    return false;
}

static BlitGameMetadata *get_metadata(const std::string &path) {

    for(auto it = metadata_cache.begin(); it != metadata_cache.end(); ++it) {
        if(it->path == path) {
            // move to top
            metadata_cache.splice(metadata_cache.begin(), metadata_cache, it);

            if(it->valid)
                return &it->data;
            
            return nullptr; // cached failure
        }
    }

    //
    debugf("load %s\n", path.c_str());
    //

    // reuse least recently used
    auto it = std::prev(metadata_cache.end());

    it->valid = parse_file_metadata(path, it->data, true);
    it->path = path;
    it->can_launch = !api.can_launch || api.can_launch(path.c_str()) == CanLaunchResult::Success;

    metadata_cache.splice(metadata_cache.begin(), metadata_cache, it);

    return it->valid ? &it->data : nullptr;
}

// gets the cached value from the metadata cache
static bool check_can_launch(const std::string &path) {
    for(auto it = metadata_cache.begin(); it != metadata_cache.end(); ++it) {
        if(it->path == path) {
            return it->can_launch;
        }
    }

    return false;
}

void init() {
    set_screen_mode(ScreenMode::hires);

    default_splash = Surface::load(asset_no_image);
    folder_splash = Surface::load(asset_folder_splash);

    metadata_cache.emplace_front(MetadataCacheEntry{});
    metadata_cache.emplace_front(MetadataCacheEntry{});
    metadata_cache.emplace_front(MetadataCacheEntry{});

    // list installed games
    if(api.list_installed_games) {
        api.list_installed_games([](const uint8_t *ptr, uint32_t block, uint32_t size){
            File::add_buffer_file("flash:/" + std::to_string(block) + ".blit", ptr, size);
        });
    }

    // restore previously selected file
    PathSave save;
    save.last_path[0] = 0;

    if(read_save(save, path_save_slot)) {
        auto split = split_path_last(save.last_path);

        if(directory_exists(std::string(split.first)) || split.first == "flash:") {
            path = split.first;
            update_file_list();

            scroll_list_to(split.second);
            return;
        }
    }

    // load default list
    update_file_list();
}

void render(uint32_t time) {

    screen.pen = Pen(20, 20, 20);
    screen.clear();

    Point center_pos(screen.bounds.w / 2, screen.bounds.h / 3);
    int full_list_width = file_list.size() * file_item_width;

    auto render_file = [&center_pos](bool is_dir, Point offset, const std::string &file_path){

        // calculate splash size/pos
        auto splash_center = offset + center_pos;
        auto splash_image = is_dir ? folder_splash : default_splash;

        float scale = 1.0f - Vec2(offset).length() / screen.bounds.w;
        Rect splash_rect{splash_center - splash_half_size * scale, splash_center + splash_half_size * scale};
    
        // skip if offscreen
        if(!screen.clip.intersects(splash_rect))
            return;

        // load metadata
        auto metadata = is_dir ? nullptr : get_metadata(file_path);

        if(metadata)
            splash_image = metadata->splash;

        // draw
        screen.alpha = scale * 255;
        screen.stretch_blit(splash_image, {Point(0, 0), splash_size}, splash_rect);
        screen.alpha = 255;

        if(metadata && !check_can_launch(file_path)) {
            // overlay incompatible message
            screen.pen = {0, 0, 0, 0xC0};
            screen.rectangle(splash_rect);

            screen.pen = {255, 0, 0};
            screen.text("INCOMPATIBLE!", launcher_font, splash_rect, true, TextAlign::center_center);
        }

        // display additional info
        // width limit partly so wrap_text doesn't blow up trying to wrap after 0 chars
        int min_w = 96;
        if(metadata && splash_rect.w > min_w) {

            float a = float(splash_rect.w - min_w) / (splash_size.w - min_w);

            auto metadata_rect = splash_rect;
            // use the space below the splash
            metadata_rect.y += splash_rect.h + 16;
            metadata_rect.h = screen.bounds.h - metadata_rect.y - 20;

            metadata_rect.deflate(4);

            auto saved_clip = screen.clip;

            // description
            screen.pen = {255, 255, 255, uint8_t(a * 255)};
            screen.clip = saved_clip.intersection(metadata_rect);
            auto wrapped_desc = screen.wrap_text(metadata->description, metadata_rect.w, launcher_font);
            screen.text(wrapped_desc, launcher_font, metadata_rect);

            // author/version
            screen.text(metadata->author + "\n" + metadata->version, launcher_font, metadata_rect, true, TextAlign::bottom_left);

            screen.clip = saved_clip;
        }

        // game title/dir name
        std::string_view label;

        if(metadata)
            label = metadata->title;
        else if(file_path == "/")
            label = "Storage";
        else if(file_path == "flash:")
            label = "Installed";
        else
            label = split_path_last(file_path).second;

        screen.pen = {255, 255, 255};
        screen.text(label, launcher_font, splash_center + Point(0, (splash_size.h / 2) * scale + 6), true, TextAlign::center_center);
    };

    int i = 0;
    for(auto &info : file_list) {
        auto file_pos = Point(i * file_item_width, 0);

        auto offset = file_pos - scroll_offset;

        // final x offset assuming unscaled image
        int full_size_off = offset.x + center_pos.x - splash_size.w / 2;

        // wrap
        if(full_size_off >= screen.bounds.w)
            offset.x -= full_list_width;
        else if(full_size_off + splash_size.w <= 0)
            offset.x += full_list_width;

        if(offset.x > screen.bounds.w || offset.x < -screen.bounds.w) {
            // skip completely offscreen
            i++;
            continue;
        }

        render_file(info.flags & FileFlags::directory, offset, join_path(path, info.name));

        i++;
    }

    // wrap two-item list
    if(file_list.size() == 2) {
        // draw first item again to the right
        auto offset = Point(full_list_width, 0) - scroll_offset;

        // ... or the left if we're wrapping from item 1 -> item 0 (moving right)
        if(scroll_offset.x < 0)
            offset.x -= full_list_width * 2;

        render_file(file_list[0].flags & FileFlags::directory, offset, join_path(path, file_list[0].name));

        // draw second item again to the left
        offset = Point(file_item_width - full_list_width, 0) - scroll_offset;
        // ... or the right if we're wrapping from item 0 -> item 1 (moving left)
        if(scroll_offset.x > file_item_width)
            offset.x += full_list_width * 2;

        render_file(file_list[1].flags & FileFlags::directory, offset, join_path(path, file_list[1].name));
    }

    // old item scrolling out vertically
    if(scroll_offset.y != 0 && !dir_change_old_path.empty()) {
        Point offset;
        offset.x = -(scroll_offset.x % file_item_width);
        offset.y = scroll_offset.y < 0 ? -screen.bounds.h - scroll_offset.y : screen.bounds.h - scroll_offset.y;

        bool is_dir = directory_exists(dir_change_old_path) || dir_change_old_path == "/" || dir_change_old_path == "flash:"; 
        render_file(is_dir, offset, dir_change_old_path);
    }

    // draw current path
    screen.pen = Pen(255, 255, 255);
    screen.rectangle(Rect(0, 0, 320, 12));

    // TODO: scroll if too long
    screen.pen = Pen(0, 0, 0);
    screen.text(path, launcher_font, Point(5, 2));

    // fade in at startup
    screen.pen = {0, 0, 0, startup_fade * 255 / startup_fade_len};
    screen.rectangle(screen.clip);

    // launch animation
    if(do_launch) {
        // slide to true center
        auto start_pos = center_pos;
        Point end_pos(screen.bounds.w / 2, screen.bounds.h / 2);

        // scale up to fill screen
        float target_scale = float(screen.bounds.w) / splash_size.w;

        // calc current pos/size
        float progress = 1.0f - (float(launch_anim_time) / launch_anim_len);

        float scale = target_scale * progress + (1.0f - progress);
        Point pos = end_pos * progress + start_pos * (1.0f - progress);

        Rect splash_rect{pos - splash_half_size * scale, pos + splash_half_size * scale};

        // get splash
        auto &current_file = file_list[file_list_offset];
        auto full_path = join_path(path, current_file.name);
        auto metadata = get_metadata(full_path);

        auto splash = metadata ? metadata->splash : default_splash;

        // draw it
        screen.alpha = progress * 255;
        screen.stretch_blit(splash, {Point{}, splash->bounds}, splash_rect);
        screen.alpha = 255;
    }
}

void update(uint32_t time) {

    // update fade
    if(startup_fade)
        startup_fade--;

    if(do_launch) {
        // update launch anim
        if(launch_anim_time == 0) {
            auto &current_file = file_list[file_list_offset];
            auto full_path = join_path(path, current_file.name);

            // launch, probably
            if(api.launch && api.launch(full_path.c_str()))
            {
                // yay!
            }
            else
            {
                // oh no
                // TODO: display error
            }

            do_launch = false;
        } else
            launch_anim_time--;

        return;
    }

    // update scroll
    int scroll_target_x = file_list_offset * file_item_width;
    if(scroll_offset.x != scroll_target_x) {
        int dir = scroll_offset.x < scroll_target_x ? 1 : -1;

        scroll_offset.x += dir * 5;
    }

    if(scroll_offset.y != 0) {
        int dir = scroll_offset.y < 0 ? 1 : -1;

        scroll_offset.y += dir * 3;
    }

    if(!file_list.empty()) {
        // list scrolling
        if(file_list.size() > 1) {
            if(buttons.released & Button::DPAD_LEFT) {
                file_list_offset--;
                if(file_list_offset < 0) {
                    file_list_offset += file_list.size();
                    // wrap scroll pos as well
                    scroll_offset.x += file_list.size() * file_item_width;
                }
            
            } else if(buttons.released & Button::DPAD_RIGHT) {
                file_list_offset++;
                if(file_list_offset >= int(file_list.size())) {
                    file_list_offset = 0;
                    scroll_offset.x -= file_list.size() * file_item_width;
                }
            }
        }

        // activate selected item
        if(buttons.released & Button::A) {
            auto &current_file = file_list[file_list_offset];

            auto full_path = join_path(path, current_file.name);

            if(current_file.flags & FileFlags::directory) {
                // navigate to dir
                path = full_path;
                update_file_list();

                dir_change_old_path = path;

                scroll_offset.y -= screen.bounds.h;
            } else if(check_can_launch(full_path)) {

                // save last file launched
                PathSave save{};
                strncpy(save.last_path, full_path.c_str(), sizeof(save.last_path) - 1);
                write_save(save, path_save_slot);

                do_launch = true;
                launch_anim_time = launch_anim_len;
            }
        }
    }

    if(buttons.released & Button::B) {
        if(!path.empty() && path != "/" && path != "flash:") {
            // go up
            dir_change_old_path = file_list.empty() ? "" : join_path(path, file_list[file_list_offset].name);

            auto split = split_path_last(path);
            auto old_dir = std::string(split.second);
            path = split.first;

            update_file_list();
            scroll_offset.y += screen.bounds.h;
            scroll_list_to(old_dir);
        } else if(!path.empty()) {
            // back to installed/storage
            dir_change_old_path = file_list.empty() ? "" : join_path(path, file_list[file_list_offset].name);

            auto old_dir = path;
            path = "";

            update_file_list();
            scroll_offset.y += screen.bounds.h;
            scroll_list_to(old_dir);
        }
    }
}