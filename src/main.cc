#include "xkit/widget.h"
#include "xkit/application.h"
#include "xkit/xbus_client.h"
#include "xkit/menu.h"
#include "xkit/toast.h"
#include "xkit/clipboard.h"
#include "shared/protocol.h"
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <cstring>
#include <cstdlib>
#include <iostream>

using namespace fbx::xkit;
using namespace fbx::shared;

// ── Shared state ───────────────────────────────────────────────────────────

struct NavState {
    std::string           current_path;
    std::vector<DirEntry> entries;
    uint32_t              total_count{0};
};

// ── Helpers ────────────────────────────────────────────────────────────────

static std::string parentOf(const std::string& path) {
    if (path == "/") return "/";
    auto pos = path.rfind('/');
    return (pos == 0) ? "/" : path.substr(0, pos);
}

static bool isSnapshotView(const std::string& path) {
    return path.find("/.zfs/snapshot") != std::string::npos;
}

// The dataset's real directory, given any path (strips /.zfs/... suffix).
static std::string datasetDirOf(const std::string& path) {
    auto pos = path.find("/.zfs/");
    return (pos != std::string::npos) ? path.substr(0, pos) : path;
}

// Fetch all pages of a directory recursively, then call on_done.
static void fetchPage(XBusClient* xbus,
                      std::shared_ptr<NavState> state,
                      const std::string& path,
                      uint32_t page,
                      std::function<void()> on_done)
{
    DirListRequest req{};
    std::strncpy(req.path_, path.c_str(), sizeof(req.path_) - 1);
    req.page_ = page;

    xbus->Call("com.freebsdx.Portal",
               static_cast<uint32_t>(PortalOpcode::ListDir),
               {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
               [xbus, state, path, page, on_done](const XBusMessage& resp) {
                   DirListing listing{};
                   std::memcpy(&listing, resp.payload_, sizeof(listing));
                   for (uint32_t i = 0; i < listing.entry_count_; ++i)
                       state->entries.push_back(listing.entries_[i]);
                   state->total_count = listing.total_count_;
                   if (listing.has_more_)
                       fetchPage(xbus, state, path, page + 1, on_done);
                   else
                       on_done();
               });
}

static void navigate(XBusClient* xbus,
                     std::shared_ptr<NavState> state,
                     const std::string& path,
                     std::function<void()> on_done)
{
    state->current_path = path;
    state->entries.clear();
    state->total_count = 0;
    fetchPage(xbus, state, path, 0, on_done);
}

// Ask the MIME service for the default handler for a file, then tell the shell
// to launch it with the file path as an argument.
static void openFile(XBusClient* xbus, const std::string& full_path,
                     std::shared_ptr<TextWidget> status_label)
{
    MimeQueryRequest req{};
    std::strncpy(req.path_, full_path.c_str(), sizeof(req.path_) - 1);
    req.is_path_ = 1;

    xbus->Call("com.freebsdx.MIME",
               static_cast<uint32_t>(MimeOpcode::QueryHandler),
               {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
               [full_path, status_label](const XBusMessage& resp) {
                   MimeHandlerList list{};
                   std::memcpy(&list, resp.payload_, sizeof(list));
                   if (list.count_ == 0) {
                       status_label->setText("No app can open this file type.");
                       return;
                   }
                   // Ask the shell to launch the handler with the file
                   ShellLaunchRequest launch{};
                   std::strncpy(launch.app_id_,   list.handler_.app_id_,
                                sizeof(launch.app_id_)   - 1);
                   std::strncpy(launch.file_path_, full_path.c_str(),
                                sizeof(launch.file_path_) - 1);
                   if (auto* bus = XApplication::getBus()) {
                       bus->Call("com.freebsdx.Shell",
                                 static_cast<uint32_t>(ShellOpcode::LaunchWithFile),
                                 {reinterpret_cast<const uint8_t*>(&launch), sizeof(launch)},
                                 nullptr);
                   }
                   status_label->setText(
                       std::string("Opening with ") + list.handler_.app_name_ + "…");
               });
}

// Send a ZFS action request and display the response in status_label.
static void zfsAction(XBusClient* xbus,
                      PortalOpcode opcode,
                      const std::string& path,
                      std::shared_ptr<TextWidget> status_label,
                      std::function<void()> on_success = nullptr)
{
    ZfsActionRequest req{};
    std::strncpy(req.path_, path.c_str(), sizeof(req.path_) - 1);

    xbus->Call("com.freebsdx.Portal",
               static_cast<uint32_t>(opcode),
               {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
               [status_label, on_success](const XBusMessage& resp) {
                   ZfsActionResponse ar{};
                   std::memcpy(&ar, resp.payload_, sizeof(ar));
                   ar.message_[sizeof(ar.message_) - 1] = '\0';
                   status_label->setText(ar.message_);
                   if (ar.success_ && on_success) on_success();
               });
}

static std::shared_ptr<Widget> makeRow(const DirEntry& entry,
                                        const std::string& full_path,
                                        std::function<void()> on_tap)
{
    std::string label = (entry.is_dir_ ? "  / " : "     ") + std::string(entry.name_);
    auto text = std::make_shared<TextWidget>(label, 15.0f);
    text->setColor(entry.is_dir_
        ? Color{0.5f, 0.8f, 1.0f, 1.0f}
        : Color{0.85f, 0.85f, 0.85f, 1.0f});

    auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<Spacer>(16, 1), text,
        std::make_shared<Spacer>(1, 1, true)   // fill the button, or it centres the name
    });
    row->setPadding(10);
    auto btn = std::make_shared<ButtonWidget>(row, on_tap);

    // The name/open area fills the width; a "⋯" button on the right opens a
    // context menu at the cursor (Open / Copy path). Kept as sibling buttons
    // rather than nesting — a ButtonWidget fires on any press within its bounds.
    std::shared_ptr<Widget> open_area = btn;
    if (!entry.is_dir_) {
        // Non-directories are draggable onto other apps (e.g. TextEditor).
        std::string captured_path = full_path;
        open_area = std::make_shared<DragSourceWidget>(
            btn, "text/uri-list", [captured_path]() { return captured_path; });
    }
    open_area->setFlexGrow(1.0f);

    std::string cap_path = full_path;
    auto open_context_menu = [cap_path, on_tap]() {
        float cx = 0.0f, cy = 0.0f;
        XApplication::cursorPosition(cx, cy);
        std::vector<MenuItem> items;
        items.push_back({"Open", on_tap});
        items.push_back({"Copy path", [cap_path]() {
            if (auto* bus = XApplication::getBus()) {
                Clipboard::setString(*bus, cap_path);
                Toast::show("Copied path");
            }
        }});
        Menu::showAt(cx, cy, std::move(items));
    };
    // Right-clicking the row also opens the context menu (in addition to "⋯").
    btn->setOnSecondaryPressed(open_context_menu);
    auto more_btn = std::make_shared<ButtonWidget>(std::string("⋯"), open_context_menu);  // U+22EF
    more_btn->setPadding(10);

    return std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        open_area, more_btn});
}

// Build a snapshot row: snapshot name on the left, Restore button on the right.
static std::shared_ptr<Widget> makeSnapshotRow(const std::string& snap_name,
                                                const std::string& snap_path,
                                                std::shared_ptr<TextWidget> status_label,
                                                std::function<void()> on_restore_done)
{
    auto name_text = std::make_shared<TextWidget>(snap_name, 14.0f);
    name_text->setColor({0.9f, 0.85f, 0.6f, 1.0f}); // warm amber for snapshots

    auto restore_btn = std::make_shared<ButtonWidget>(
        std::make_shared<TextWidget>("Restore", 13.0f),
        [snap_path, status_label, on_restore_done]() {
            auto* xbus = XApplication::getBus();
            if (!xbus) return;
            status_label->setText("Rolling back…");
            zfsAction(xbus, PortalOpcode::ZfsRollback, snap_path,
                      status_label, on_restore_done);
        });
    restore_btn->setPadding(6);

    auto spacer = std::make_shared<Spacer>(1, 1, /*flexible=*/true);

    auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<Spacer>(16, 1),
        name_text,
        spacer,
        restore_btn,
        std::make_shared<Spacer>(12, 1)
    });
    row->setPadding(10);
    return row;
}

// ── main ──────────────────────────────────────────────────────────────────

int main() {
    const char* home_cstr = std::getenv("HOME");
    std::string home = home_cstr ? home_cstr : "/tmp";

    auto state = std::make_shared<NavState>();
    state->current_path = home;

    auto path_label   = std::make_shared<TextWidget>(home, 13.0f);
    path_label->setColor({0.5f, 0.8f, 1.0f, 1.0f});

    auto entry_list   = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});
    auto status_label = std::make_shared<TextWidget>("Loading…", 12.0f);
    status_label->setColor({0.55f, 0.55f, 0.55f, 1.0f});

    // Use shared_ptr-to-function so rebuild can capture itself recursively
    auto rebuild = std::make_shared<std::function<void()>>();

    *rebuild = [state, entry_list, path_label, status_label, rebuild]() {
        path_label->setText(state->current_path);
        entry_list->clearChildren();

        bool in_snapshots = isSnapshotView(state->current_path);

        for (const auto& e : state->entries) {
            std::string name(e.name_);
            bool is_dir = (e.is_dir_ != 0);
            std::string full = state->current_path + "/" + name;

            if (in_snapshots) {
                // Snapshot view: show Restore button next to each snapshot
                auto row = makeSnapshotRow(name, full, status_label, [rebuild]() {
                    if (*rebuild) (*rebuild)();
                });
                entry_list->addChild(row);
            } else {
                auto btn = makeRow(e, full, [state, rebuild, is_dir, full,
                                             status_label]() mutable {
                    auto* xbus = XApplication::getBus();
                    if (!xbus) return;
                    if (is_dir) {
                        status_label->setText("Loading…");
                        navigate(xbus, state, full, [rebuild]() {
                            if (*rebuild) (*rebuild)();
                        });
                    } else {
                        openFile(xbus, full, status_label);
                    }
                });
                entry_list->addChild(btn);
            }
        }

        if (state->entries.empty())
            entry_list->addChild(std::make_shared<TextWidget>(
                in_snapshots ? "(no snapshots)" : "(empty)", 14.0f));

        if (!in_snapshots)
            status_label->setText(std::to_string(state->total_count) + (state->total_count == 1 ? " item" : " items"));
    };

    // ── Navigation callbacks ──────────────────────────────────────────────
    auto go_up = [state, status_label, rebuild]() {
        auto* xbus = XApplication::getBus();
        if (!xbus) return;
        std::string parent = parentOf(state->current_path);
        if (parent == state->current_path) return;
        status_label->setText("Loading…");
        navigate(xbus, state, parent, [rebuild]() {
            if (*rebuild) (*rebuild)();
        });
    };

    auto go_snapshots = [state, status_label, rebuild]() {
        auto* xbus = XApplication::getBus();
        if (!xbus) return;
        // Navigate to the .zfs/snapshot virtual directory of the current dataset root
        std::string dataset_dir = datasetDirOf(state->current_path);
        std::string snap_dir = dataset_dir + "/.zfs/snapshot";
        status_label->setText("Loading snapshots…");
        navigate(xbus, state, snap_dir, [rebuild]() {
            if (*rebuild) (*rebuild)();
        });
    };

    auto take_snapshot = [state, status_label, rebuild]() {
        auto* xbus = XApplication::getBus();
        if (!xbus) return;
        std::string dataset_dir = datasetDirOf(state->current_path);
        status_label->setText("Creating snapshot…");
        zfsAction(xbus, PortalOpcode::ZfsSnapshot, dataset_dir, status_label,
                  [rebuild]() { if (*rebuild) (*rebuild)(); });
    };

    // ── Header widgets ────────────────────────────────────────────────────
    auto up_btn = std::make_shared<ButtonWidget>(
        std::make_shared<TextWidget>("↑  Up", 14.0f),
        go_up);
    up_btn->setPadding(8);

    auto snapshots_btn = std::make_shared<ButtonWidget>(
        std::make_shared<TextWidget>("Snapshots", 14.0f),
        go_snapshots);
    snapshots_btn->setPadding(8);

    auto take_snap_btn = std::make_shared<ButtonWidget>(
        std::make_shared<TextWidget>("+ Snapshot", 14.0f),
        take_snapshot);
    take_snap_btn->setPadding(8);

    // Normal file browser header
    auto file_header = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        up_btn,
        std::make_shared<Spacer>(12, 1),
        path_label,
        std::make_shared<Spacer>(1, 1, /*flexible=*/true),
        snapshots_btn,
    });
    file_header->setPadding(12);
    file_header->setBackgroundColor({0.1f, 0.12f, 0.18f, 1.0f});

    // Snapshot browser header (shown when in .zfs/snapshot view)
    auto snap_header = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        up_btn,
        std::make_shared<Spacer>(12, 1),
        std::make_shared<TextWidget>("Snapshots", 14.0f),
        std::make_shared<Spacer>(1, 1, /*flexible=*/true),
        take_snap_btn,
    });
    snap_header->setPadding(12);
    snap_header->setBackgroundColor({0.08f, 0.12f, 0.16f, 1.0f});

    // Stack both headers; we swap visibility in rebuild.
    // Simplest approach: use a single mutable header container — just use StackWidget
    // and control content via rebuild. Instead, use a FlexColumn with two rows and
    // show only the relevant one based on path. Since visibility toggling isn't
    // implemented, we switch header content by using a single FlexRow that rebuild
    // mutates. Use a ContainerWidget approach: wrap in a FlexColumn[1] and replace.

    // Simpler: one header_col that rebuild repopulates each time.
    auto header_col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        file_header
    });
    header_col->setBackgroundColor({0.1f, 0.12f, 0.18f, 1.0f});

    // Update rebuild to also swap the header
    *rebuild = [state, entry_list, path_label, status_label, rebuild,
                header_col, file_header, snap_header]() {
        path_label->setText(state->current_path);
        entry_list->clearChildren();
        header_col->clearChildren();

        bool in_snapshots = isSnapshotView(state->current_path);
        header_col->addChild(in_snapshots ? snap_header : file_header);

        for (const auto& e : state->entries) {
            std::string name(e.name_);
            bool is_dir = (e.is_dir_ != 0);
            std::string full = state->current_path + "/" + name;

            if (in_snapshots) {
                auto row = makeSnapshotRow(name, full, status_label, [rebuild]() {
                    if (*rebuild) (*rebuild)();
                });
                entry_list->addChild(row);
            } else {
                auto btn = makeRow(e, full, [state, rebuild, is_dir, full,
                                             status_label]() mutable {
                    auto* xbus = XApplication::getBus();
                    if (!xbus) return;
                    if (is_dir) {
                        status_label->setText("Loading…");
                        navigate(xbus, state, full, [rebuild]() {
                            if (*rebuild) (*rebuild)();
                        });
                    } else {
                        openFile(xbus, full, status_label);
                    }
                });
                entry_list->addChild(btn);
            }
        }

        if (state->entries.empty())
            entry_list->addChild(std::make_shared<TextWidget>(
                in_snapshots ? "(no snapshots)" : "(empty)", 14.0f));

        if (!in_snapshots)
            status_label->setText(std::to_string(state->total_count) + (state->total_count == 1 ? " item" : " items"));
        else if (state->total_count > 0)
            status_label->setText(std::to_string(state->total_count) + " snapshots");
    };

    auto footer = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<Spacer>(16, 1), status_label,
    });
    footer->setPadding(8);
    footer->setBackgroundColor({0.07f, 0.07f, 0.1f, 1.0f});

    auto scroll = std::make_shared<ScrollViewWidget>(entry_list);
    auto root   = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        header_col, scroll, footer,
    });
    root->setBackgroundColor({0.08f, 0.1f, 0.14f, 1.0f});

    return XApplication::Run(root, "/tmp/freebsdx.sock", false, false,
        [state, status_label, rebuild](XBusClient& xbus) mutable {
            XApplication::setTitle("Files");
            navigate(&xbus, state, state->current_path, [rebuild]() {
                if (*rebuild) (*rebuild)();
            });
        });
}
