/*  Clipboard - Cut, copy, and paste anything, anywhere, all from the terminal.
    Copyright (C) 2023 Jackson Huff and other contributors on GitHub.com
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.*/
#include "clipboard.hpp"
#include <algorithm>
#include <regex>

#if defined(_WIN32) || defined(_WIN64)
#include <fcntl.h>
#include <io.h>
#endif

namespace PerformAction {

void copyItem(const fs::path& f) {
    auto actuallyCopyItem = [&](const bool use_regular_copy = copying.use_safe_copy) {
        if (fs::is_directory(f)) {
            auto target = f.filename().empty() ? f.parent_path().filename() : f.filename();
            fs::create_directories(path.data / target);
            fs::copy(f, path.data / target, copying.opts);
        } else {
            fs::copy(f, path.data / f.filename(), use_regular_copy ? copying.opts : copying.opts | fs::copy_options::create_hard_links);
        }
        incrementSuccessesForItem(f);
        if (action == Action::Cut) writeToFile(path.metadata.originals, fs::absolute(f).string() + "\n", true);
    };
    try {
        actuallyCopyItem();
    } catch (const fs::filesystem_error& e) {
        if (!copying.use_safe_copy && e.code() == std::errc::cross_device_link) {
            try {
                actuallyCopyItem(true);
            } catch (const fs::filesystem_error& e) {
                copying.failedItems.emplace_back(f.string(), e.code());
            }
        } else {
            copying.failedItems.emplace_back(f.string(), e.code());
        }
    }
}

void copy() {
    for (const auto& f : copying.items)
        copyItem(f);
}

void copyText() {
    copying.buffer = copying.items.at(0).string();
    writeToFile(path.data.raw, copying.buffer);

    if (!output_silent) {
        printf(replaceColors("[success]✅ %s text \"[bold]%s[blank][success]\"[blank]\n").data(), did_action[action].data(), copying.buffer.data());
    }

    if (action == Action::Cut) writeToFile(path.metadata.originals, path.data.raw.string());
    successes.bytes = 0; // temporarily disable the bytes success message
}

void paste() {
    for (const auto& f : fs::directory_iterator(path.data)) {
        auto target = fs::current_path() / f.path().filename();
        auto pasteItem = [&](const bool use_regular_copy = copying.use_safe_copy) {
            if (!(fs::exists(target) && fs::equivalent(f, target))) {
                fs::copy(f, target, use_regular_copy || fs::is_directory(f) ? copying.opts : copying.opts | fs::copy_options::create_hard_links);
            }
            incrementSuccessesForItem(f);
        };
        try {
            if (fs::exists(target)) {
                using enum CopyPolicy;
                switch (copying.policy) {
                case SkipAll:
                    break;
                case ReplaceAll:
                    pasteItem();
                    break;
                default:
                    stopIndicator();
                    copying.policy = userDecision(f.path().filename().string());
                    startIndicator();
                    if (copying.policy == ReplaceOnce || copying.policy == ReplaceAll) {
                        pasteItem();
                    }
                    break;
                }
            } else {
                pasteItem();
            }
        } catch (const fs::filesystem_error& e) {
            if (!copying.use_safe_copy) {
                try {
                    pasteItem(true);
                } catch (const fs::filesystem_error& e) {
                    copying.failedItems.emplace_back(f.path().filename().string(), e.code());
                }
            } else {
                copying.failedItems.emplace_back(f.path().filename().string(), e.code());
            }
        }
    }
    removeOldFiles();
}

void pipeIn() {
    copying.buffer = pipedInContent();
    writeToFile(path.data.raw, copying.buffer);
    if (action == Action::Cut) writeToFile(path.metadata.originals, path.data.raw.string());
}

void pipeOut() {
    for (const auto& entry : fs::recursive_directory_iterator(path.data)) {
        std::string content(fileContents(entry.path()));
#if !defined(_WIN32) && !defined(_WIN64)
        int len = write(fileno(stdout), content.data(), content.size());
        if (len < 0) throw std::runtime_error("write() failed");
#elif defined(_WIN32) || defined(_WIN64)
        _setmode(_fileno(stdout), _O_BINARY);
        fwrite(content.data(), sizeof(char), content.size(), stdout);
#endif
        fflush(stdout);
        successes.bytes += content.size();
    }
    removeOldFiles();
}

void clear() {
    if (fs::is_empty(path.data)) printf("%s", no_clipboard_contents_message().data());
    clearTempDirectory(true);
}

void show() {
    stopIndicator();
    if (fs::is_directory(path.data) && !fs::is_empty(path.data)) {
        if (fs::is_regular_file(path.data.raw)) {
            std::string content(fileContents(path.data.raw));
            std::erase(content, '\n');
            printf(clipboard_text_contents_message().data(), std::min(static_cast<size_t>(250), content.size()), clipboard_name.data());
            printf(replaceColors("[bold][info]%s\n[blank]").data(), content.substr(0, 250).data());
            if (content.size() > 250) {
                printf(and_more_items_message().data(), content.size() - 250);
            }
            return;
        }
        size_t total_items = 0;

        for (auto dummy : fs::directory_iterator(path.data))
            total_items++;

        size_t rowsAvailable = thisTerminalSize().rows - (clipboard_item_many_contents_message.rawLength() / thisTerminalSize().columns);
        rowsAvailable -= 3;
        printf(total_items > rowsAvailable ? clipboard_item_too_many_contents_message().data() : clipboard_item_many_contents_message().data(),
               std::min(rowsAvailable, total_items),
               clipboard_name.data());
        auto it = fs::directory_iterator(path.data);
        for (size_t i = 0; i < std::min(rowsAvailable, total_items); i++) {

            printf(replaceColors("[info]▏ [bold][help]%s[blank]\n").data(), it->path().filename().string().data());

            if (i == rowsAvailable - 1 && total_items > rowsAvailable) printf(and_more_items_message().data(), total_items - rowsAvailable);

            it++;
        }
    } else {
        printf(no_clipboard_contents_message().data(), actions[Action::Cut].data(), actions[Action::Copy].data(), actions[Action::Paste].data(), actions[Action::Copy].data());
    }
}

void edit() {}

void addFiles() {
    if (fs::is_regular_file(path.data.raw)) {
        fprintf(stderr,
                "%s",
                replaceColors("[error]❌ You can't add items to text. [blank][help]Try copying text first, or add "
                              "text instead.[blank]\n")
                        .data());
        exit(EXIT_FAILURE);
    }
    for (const auto& f : copying.items)
        copyItem(f);
}

void addData() {
    if (fs::is_regular_file(path.data.raw)) {
        std::string content;
        if (io_type == IOType::Pipe)
            content = pipedInContent();
        else
            content = copying.items.at(0).string();
        successes.bytes += writeToFile(path.data.raw, content, true);
    } else if (!fs::is_empty(path.data)) {
        fprintf(stderr,
                "%s",
                replaceColors("[error]❌ You can't add text to items. [blank][help]Try copying text first, or add a "
                              "file instead.[blank]\n")
                        .data());
        exit(EXIT_FAILURE);
    } else {
        if (io_type == IOType::Pipe)
            pipeIn();
        else if (io_type == IOType::Text)
            successes.bytes += writeToFile(path.data.raw, copying.items.at(0).string());
    }
}

void removeRegex() {
    std::vector<std::regex> regexes;
    if (io_type == IOType::Pipe)
        regexes.emplace_back(pipedInContent());
    else
        std::transform(copying.items.begin(), copying.items.end(), std::back_inserter(regexes), [](const auto& item) { return std::regex(item.string()); });

    if (fs::is_regular_file(path.data.raw)) {
        std::string content(fileContents(path.data.raw));
        size_t oldLength = content.size();

        for (const auto& pattern : regexes)
            content = std::regex_replace(content, pattern, "");
        successes.bytes += oldLength - content.size();

        if (oldLength != content.size()) {
            writeToFile(path.data.raw, content);
        } else {
            fprintf(stderr,
                    "%s",
                    replaceColors("[error]❌ Clipboard couldn't match your pattern(s) against anything. [blank][help]Try using a different pattern instead or check what's "
                                  "stored.[blank]\n")
                            .data());
            exit(EXIT_FAILURE);
        }
    } else {
        for (const auto& entry : fs::directory_iterator(path.data)) {
            for (const auto& pattern : regexes) {
                if (std::regex_match(entry.path().filename().string(), pattern)) {
                    try {
                        fs::remove_all(entry.path());
                        incrementSuccessesForItem(entry.path());
                    } catch (const fs::filesystem_error& e) {
                        copying.failedItems.emplace_back(entry.path().filename().string(), e.code());
                    }
                }
            }
        }
        if (successes.directories == 0 && successes.files == 0) {
            fprintf(stderr,
                    "%s",
                    replaceColors("[error]❌ Clipboard couldn't match your pattern(s) against anything. [blank][help]Try using a different pattern instead or check what's "
                                  "stored.[blank]\n")
                            .data());
            exit(EXIT_FAILURE);
        }
    }
}

void noteText() {
    if (copying.items.size() == 1) {
        if (copying.items.at(0).string() == "") {
            fs::remove(path.metadata.notes);
            if (output_silent) return;
            stopIndicator();
            fprintf(stderr, "%s", replaceColors("[success]✅ Removed note\n").data());
        } else {
            writeToFile(path.metadata.notes, copying.items.at(0).string());
            if (output_silent) return;
            stopIndicator();
            fprintf(stderr, replaceColors("[success]✅ Saved note \"%s\"\n").data(), copying.items.at(0).string().data());
        }
    } else if (copying.items.empty()) {
        if (fs::is_regular_file(path.metadata.notes)) {
            std::string content(fileContents(path.metadata.notes));
            if (is_tty.out)
                fprintf(stdout, replaceColors("[info]• Note for this clipboard: %s\n").data(), content.data());
            else
                fprintf(stdout, replaceColors("%s").data(), content.data());
        } else {
            fprintf(stderr, "%s", replaceColors("[info]• There is no note for this clipboard.[blank]\n").data());
        }
    } else {
        fprintf(stderr, "%s", replaceColors("[error]❌ You can't add multiple items to a note. [blank][help]Try providing a single piece of text instead.[blank]\n").data());
        exit(EXIT_FAILURE);
    }
}

void notePipe() {
    std::string content(pipedInContent());
    writeToFile(path.metadata.notes, content);
    if (output_silent) return;
    stopIndicator();
    fprintf(stderr, replaceColors("[success]✅ Saved note \"%s\"\n").data(), content.data());
    exit(EXIT_SUCCESS);
}

void swap() {}

void status() {
    syncWithGUIClipboard(true);
    stopIndicator();
    std::vector<std::pair<fs::path, bool>> clipboards_with_contents;
    auto iterateClipboards = [&](const fs::path& path, bool persistent) { // use zip ranges here when gcc 13 comes out
        for (const auto& entry : fs::directory_iterator(path))
            if (fs::exists(entry.path() / constants.data_directory) && !fs::is_empty(entry.path() / constants.data_directory))
                clipboards_with_contents.push_back({entry.path(), persistent});
    };
    iterateClipboards(global_path.temporary, false);
    iterateClipboards(global_path.persistent, true);
    std::sort(clipboards_with_contents.begin(), clipboards_with_contents.end());

    if (clipboards_with_contents.empty()) {
        printf("%s", no_clipboard_contents_message().data());
        printf("%s", clipboard_action_prompt().data());
    } else {
        TerminalSize available(thisTerminalSize());

        available.rows -= check_clipboard_status_message.rawLength() / available.columns;
        if (clipboards_with_contents.size() > available.rows) available.rows -= and_more_items_message.rawLength() / available.columns;

        printf("%s", check_clipboard_status_message().data());

        for (size_t clipboard = 0; clipboard < std::min(clipboards_with_contents.size(), available.rows); clipboard++) {

            int widthRemaining = available.columns
                                 - (clipboards_with_contents.at(clipboard).first.filename().string().length() + 4
                                    + std::string_view(clipboards_with_contents.at(clipboard).second ? " (p)" : "").length());
            printf(replaceColors("[bold][info]▏ %s%s: [blank]").data(),
                   clipboards_with_contents.at(clipboard).first.filename().string().data(),
                   clipboards_with_contents.at(clipboard).second ? " (p)" : "");

            if (fs::is_regular_file(clipboards_with_contents.at(clipboard).first / constants.data_directory / constants.data_file_name)) {
                std::string content(fileContents(clipboards_with_contents.at(clipboard).first / constants.data_directory / constants.data_file_name));
                std::erase(content, '\n');
                printf(replaceColors("[help]%s[blank]\n").data(), content.substr(0, widthRemaining).data());
                continue;
            }

            for (bool first = true; const auto& entry : fs::directory_iterator(clipboards_with_contents.at(clipboard).first / constants.data_directory)) {
                int entryWidth = entry.path().filename().string().length();

                if (widthRemaining <= 0) break;

                if (!first) {
                    if (entryWidth <= widthRemaining - 2) {
                        printf("%s", replaceColors("[help], [blank]").data());
                        widthRemaining -= 2;
                    }
                }

                if (entryWidth <= widthRemaining) {
                    printf(replaceColors("[help]%s[blank]").data(), entry.path().filename().string().data());
                    widthRemaining -= entryWidth;
                    first = false;
                }
            }
            printf("\n");
        }
        if (clipboards_with_contents.size() > available.rows) printf(and_more_items_message().data(), clipboards_with_contents.size() - available.rows);
    }
}

void info() {
    // display the following information:
    // clipboard name
    // number of files and directories or bytes
    // last write time
    // filesystem location
    // locked or not and the pid of the locking process
    // saved note

    fprintf(stderr, replaceColors("[info]• Clipboard name: [help]%s[blank]\n").data(), clipboard_name.data());
}
} // namespace PerformAction