/*  The Clipboard Project - Cut, copy, and paste anything, anywhere, all from the terminal.
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
#include <fstream>
#include <regex>

#if defined(_WIN32) || defined(_WIN64)
#include <fcntl.h>
#include <io.h>
#endif

#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <sys/types.h>
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
        printf(formatMessage("[success]✅ %s text \"[bold]%s[blank][success]\"[blank]\n").data(), did_action[action].data(), copying.buffer.data());
    }

    if (action == Action::Cut) writeToFile(path.metadata.originals, path.data.raw.string());
    successes.bytes = 0; // temporarily disable the bytes success message
}

void paste() {
    std::vector<std::regex> regexes;
    if (!copying.items.empty()) {
        std::transform(copying.items.begin(), copying.items.end(), std::back_inserter(regexes), [](const auto& item) { return std::regex(item.string()); });
    }

    for (const auto& entry : fs::directory_iterator(path.data)) {
        auto target = fs::current_path() / entry.path().filename();
        auto pasteItem = [&](const bool use_regular_copy = copying.use_safe_copy) {
            if (!(fs::exists(target) && fs::equivalent(entry, target))) {
                fs::copy(entry, target, use_regular_copy || fs::is_directory(entry) ? copying.opts : copying.opts | fs::copy_options::create_hard_links);
            }
            incrementSuccessesForItem(entry);
        };
        if (!regexes.empty() && !std::any_of(regexes.begin(), regexes.end(), [&](const auto& regex) { return std::regex_match(entry.path().filename().string(), regex); }))
            continue;
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
                    copying.policy = userDecision(entry.path().filename().string());
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
                    copying.failedItems.emplace_back(entry.path().filename().string(), e.code());
                }
            } else {
                copying.failedItems.emplace_back(entry.path().filename().string(), e.code());
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
    if (all_option) {
        if (!userIsARobot()) {
            stopIndicator();
            fprintf(stderr,
                    formatMessage("[progress]🟡 Are you sure you want to clear all clipboards?[blank] [help]This will remove everything in locations [bold]%s[blank][help] and "
                                  "[bold]%s[blank][help]. [bold][y(es)/n(o)] ")
                            .data(),
                    global_path.temporary.string().data(),
                    global_path.persistent.string().data());
            std::string decision;
            std::getline(std::cin, decision);
            int clipboards_cleared = 0;
            if (decision.substr(0, 1) == "y" || decision.substr(0, 1) == "Y") {
                for (const auto& entry : fs::directory_iterator(global_path.temporary)) {
                    bool predicate = Clipboard(entry.path().filename().string()).holdsData();
                    fs::remove_all(entry);
                    if (predicate) clipboards_cleared++;
                }
                for (const auto& entry : fs::directory_iterator(global_path.persistent)) {
                    bool predicate = Clipboard(entry.path().filename().string()).holdsData();
                    fs::remove_all(entry);
                    if (predicate) clipboards_cleared++;
                }
            }
            fprintf(stderr, "%s", formatMessage("[blank]").data());
            fprintf(stderr, formatMessage("[success]✅ Cleared %d clipboard%s[blank]\n").data(), clipboards_cleared, clipboards_cleared == 1 ? "" : "s");
        }
    } else {
        clearData(true);
    }
}

void show() {
    std::vector<std::regex> regexes;
    if (!copying.items.empty()) {
        std::transform(copying.items.begin(), copying.items.end(), std::back_inserter(regexes), [](const auto& item) { return std::regex(item.string()); });
    }

    stopIndicator();

    if (path.holdsRawData()) {
        std::string content(fileContents(path.data.raw));
        std::erase(content, '\n');
        printf(clipboard_text_contents_message().data(), std::min(static_cast<size_t>(250), content.size()), clipboard_name.data());
        printf(formatMessage("[bold][info]%s\n[blank]").data(), content.substr(0, 250).data());
        if (content.size() > 250) {
            printf(and_more_items_message().data(), content.size() - 250);
        }
        return;
    }

    printf(clipboard_item_many_contents_message().data(), clipboard_name.data());

    for (const auto& entry : fs::directory_iterator(path.data)) {
        if (!regexes.empty() && !std::any_of(regexes.begin(), regexes.end(), [&](const auto& regex) { return std::regex_match(entry.path().filename().string(), regex); }))
            continue;
        printf(formatMessage("[info]│ [bold][help]%s[blank]\n").data(), entry.path().filename().string().data());
    }
}

void showFilepaths() {
    std::vector<std::regex> regexes;
    if (!copying.items.empty()) {
        std::transform(copying.items.begin(), copying.items.end(), std::back_inserter(regexes), [](const auto& item) { return std::regex(item.string()); });
    }

    std::vector<fs::path> paths(fs::directory_iterator(path.data), fs::directory_iterator {});
    if (!regexes.empty())
        paths.erase(
                std::remove_if(
                        paths.begin(),
                        paths.end(),
                        [&](const auto& entry) {
                            return !std::any_of(regexes.begin(), regexes.end(), [&](const auto& regex) { return std::regex_match(entry.filename().string(), regex); });
                        }
                ),
                paths.end()
        );

    for (const auto& entry : paths) {
        printf("\"%s\"", entry.string().data());
        incrementSuccessesForItem(entry);
        if (entry != paths.back()) printf(" ");
    }
}

void edit() {}

void addFiles() {
    if (path.holdsRawData()) {
        fprintf(stderr,
                "%s",
                formatMessage("[error]❌ You can't add items to text. [blank][help]Try copying text first, or add "
                              "text instead.[blank]\n")
                        .data());
        exit(EXIT_FAILURE);
    }
    for (const auto& f : copying.items)
        copyItem(f);
}

void addData() {
    if (path.holdsRawData()) {
        std::string content;
        if (io_type == IOType::Pipe)
            content = pipedInContent();
        else
            content = copying.items.at(0).string();
        successes.bytes += writeToFile(path.data.raw, content, true);
    } else if (!fs::is_empty(path.data)) {
        fprintf(stderr,
                "%s",
                formatMessage("[error]❌ You can't add text to items. [blank][help]Try copying text first, or add a "
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

    if (path.holdsRawData()) {
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
                    formatMessage("[error]❌ CB couldn't match your pattern(s) against anything. [blank][help]Try using a different pattern instead or check what's "
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
                    formatMessage("[error]❌ CB couldn't match your pattern(s) against anything. [blank][help]Try using a different pattern instead or check what's "
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
            fprintf(stderr, "%s", formatMessage("[success]✅ Removed note\n").data());
        } else {
            writeToFile(path.metadata.notes, copying.items.at(0).string());
            if (output_silent) return;
            stopIndicator();
            fprintf(stderr, formatMessage("[success]✅ Saved note \"%s\"\n").data(), copying.items.at(0).string().data());
        }
    } else if (copying.items.empty()) {
        if (fs::is_regular_file(path.metadata.notes)) {
            std::string content(fileContents(path.metadata.notes));
            if (is_tty.out)
                fprintf(stdout, formatMessage("[info]🔷 Note for this clipboard: %s\n").data(), content.data());
            else
                fprintf(stdout, formatMessage("%s").data(), content.data());
        } else {
            fprintf(stderr, "%s", formatMessage("[info]🔷 There is no note for this clipboard.[blank]\n").data());
        }
    } else {
        fprintf(stderr, "%s", formatMessage("[error]❌ You can't add multiple items to a note. [blank][help]Try providing a single piece of text instead.[blank]\n").data());
        exit(EXIT_FAILURE);
    }
}

void notePipe() {
    std::string content(pipedInContent());
    writeToFile(path.metadata.notes, content);
    if (output_silent) return;
    stopIndicator();
    fprintf(stderr, formatMessage("[success]✅ Saved note \"%s\"\n").data(), content.data());
    exit(EXIT_SUCCESS);
}

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
        printf(clipboard_action_prompt().data(), clipboard_invocation.data(), clipboard_invocation.data());
    } else {
        TerminalSize available(thisTerminalSize());

        available.rows -= check_clipboard_status_message.rawLength() / available.columns;
        if (clipboards_with_contents.size() > available.rows) available.rows -= and_more_items_message.rawLength() / available.columns;

        printf("%s", check_clipboard_status_message().data());

        for (size_t clipboard = 0; clipboard < std::min(clipboards_with_contents.size(), available.rows); clipboard++) {

            int widthRemaining = available.columns
                                 - (clipboards_with_contents.at(clipboard).first.filename().string().length() + 4
                                    + std::string_view(clipboards_with_contents.at(clipboard).second ? " (p)" : "").length());
            printf(formatMessage("[bold][info]│ %s%s: [blank]").data(),
                   clipboards_with_contents.at(clipboard).first.filename().string().data(),
                   clipboards_with_contents.at(clipboard).second ? " (p)" : "");

            if (fs::is_regular_file(clipboards_with_contents.at(clipboard).first / constants.data_directory / constants.data_file_name)) {
                std::string content(fileContents(clipboards_with_contents.at(clipboard).first / constants.data_directory / constants.data_file_name));
                std::erase(content, '\n');
                printf(formatMessage("[help]%s[blank]\n").data(), content.substr(0, widthRemaining).data());
                continue;
            }

            for (bool first = true; const auto& entry : fs::directory_iterator(clipboards_with_contents.at(clipboard).first / constants.data_directory)) {
                int entryWidth = entry.path().filename().string().length();

                if (widthRemaining <= 0) break;

                if (!first) {
                    if (entryWidth <= widthRemaining - 2) {
                        printf("%s", formatMessage("[help], [blank]").data());
                        widthRemaining -= 2;
                    }
                }

                if (entryWidth <= widthRemaining) {
                    printf(formatMessage("[help]%s[blank]").data(), entry.path().filename().string().data());
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
    auto available(thisTerminalSize());
    available.columns -= 4;                                                                  // for box corners
    available.columns -= (clipboard_name_message.rawLength() - 2) + clipboard_name.length(); // compensate for %s and clipboard name
    bool addExtraBar = available.columns % 2 != 0;
    available.columns /= 2;
    fprintf(stderr, "%s", formatMessage("[info]┍").data());
    for (int i = 0; i < available.columns; i++)
        fprintf(stderr, "━");
    fprintf(stderr, "┫");
    fprintf(stderr, clipboard_name_message().data(), clipboard_name.data());
    fprintf(stderr, "%s", formatMessage("[info]┣").data());
    for (int i = 0; i < available.columns; i++)
        fprintf(stderr, "%s", formatMessage("[info]━").data());
    if (addExtraBar) fprintf(stderr, "━");
    fprintf(stderr, "%s", formatMessage("┑[blank]\n").data());

    auto total_cols = thisTerminalSize().columns;

    auto displayEndbar = [&]() {
        fprintf(stderr, "\033[%ldG%s\r", total_cols, formatMessage("[info]│[blank]").data());
    };

#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)

    struct stat info;
    stat(path.string().data(), &info);
    std::string time(std::ctime(&info.st_ctime));
    std::erase(time, '\n');

    displayEndbar();
    fprintf(stderr, formatMessage("[info]│ Last changed [help]%s[blank]\n").data(), time.data());

#endif
    displayEndbar();
    fprintf(stderr, formatMessage("[info]│ Stored in [help]%s[blank]\n").data(), path.string().data());
    displayEndbar();
    fprintf(stderr, formatMessage("[info]│ Persistent? [help]%s[blank]\n").data(), path.is_persistent ? "Yes" : "No");

    if (path.holdsRawData()) {
        displayEndbar();
        fprintf(stderr, formatMessage("[info]│ Bytes: [help]%s[blank]\n").data(), formatBytes(fs::file_size(path.data.raw)).data());
        displayEndbar();
        fprintf(stderr, formatMessage("[info]│ Content type: [help]%s[blank]\n").data(), inferMIMEType(fileContents(path.data.raw)).value_or("(Unknown)").data());
    } else {
        size_t files = 0;
        size_t directories = 0;
        for (const auto& entry : fs::directory_iterator(path.data)) {
            if (fs::is_directory(entry.path()))
                directories++;
            else
                files++;
        }
        displayEndbar();
        fprintf(stderr, formatMessage("[info]│ Files: [help]%zu[blank]\n").data(), files);
        displayEndbar();
        fprintf(stderr, formatMessage("[info]│ Directories: [help]%zu[blank]\n").data(), directories);
    }

    if (!available_mimes.empty()) {
        displayEndbar();
        fprintf(stderr, "%s", formatMessage("[info]│ Available types from GUI: [help]").data());
        for (const auto& mime : available_mimes) {
            fprintf(stderr, "%s", mime.data());
            if (mime != available_mimes.back()) fprintf(stderr, ", ");
        }
        fprintf(stderr, "%s", formatMessage("[blank]\n").data());
    }
    displayEndbar();
    fprintf(stderr, formatMessage("[info]│ Content cut? [help]%s[blank]\n").data(), fs::exists(path.metadata.originals) ? "Yes" : "No");

    displayEndbar();
    fprintf(stderr, formatMessage("[info]│ Locked by another process? [help]%s[blank]\n").data(), path.isLocked() ? "Yes" : "No");

    if (path.isLocked()) {
        displayEndbar();
        fprintf(stderr, formatMessage("[info]│ Locked by process with pid [help]%s[blank]\n").data(), fileContents(path.metadata.lock).data());
    }

    displayEndbar();
    if (fs::exists(path.metadata.notes))
        fprintf(stderr, formatMessage("[info]│ Note: [help]%s[blank]\n").data(), fileContents(path.metadata.notes).data());
    else
        fprintf(stderr, "%s", formatMessage("[info]│ There is no note for this clipboard.[blank]\n").data());

    fprintf(stderr, "%s", formatMessage("[info]┕").data());
    auto avail_cols = thisTerminalSize().columns;
    for (int i = 0; i < avail_cols - 2; i++)
        fprintf(stderr, "━");
    fprintf(stderr, "%s", formatMessage("┙[blank]\n").data());
}

void infoJSON() {
    printf("{\n");

    printf("    \"name\": \"%s\",\n", clipboard_name.data());

#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)

    struct stat info;
    stat(path.string().data(), &info);
    std::string time(std::ctime(&info.st_ctime));
    std::erase(time, '\n');

    printf("    \"lastChanged\": \"%s\",\n", time.data());

#endif

    printf("    \"path\": \"%s\",\n", path.string().data());
    printf("    \"isPersistent\": %s,\n", path.is_persistent ? "true" : "false");

    if (path.holdsRawData()) {
        printf("    \"bytes\": \"%s\",\n", std::to_string(fs::file_size(path.data.raw)).data());
        printf("    \"contentType\": \"%s\",\n", inferMIMEType(fileContents(path.data.raw)).value_or("(Unknown)").data());
    } else {
        size_t files = 0;
        size_t directories = 0;
        for (const auto& entry : fs::directory_iterator(path.data)) {
            if (fs::is_directory(entry.path()))
                directories++;
            else
                files++;
        }
        printf("    \"files\": \"%zu\",\n", files);
        printf("    \"directories\": \"%zu\",\n", directories);
    }

    if (!available_mimes.empty()) {
        printf("    \"availableTypes\": [");
        for (const auto& mime : available_mimes) {
            printf("\"%s\"", mime.data());
            if (mime != available_mimes.back()) printf(", ");
        }
        printf("],\n");
    }

    printf("    \"contentCut\": %s,\n", fs::exists(path.metadata.originals) ? "true" : "false");

    printf("    \"locked\": %s,\n", path.isLocked() ? "true" : "false");
    if (path.isLocked()) {
        printf("    \"lockedBy\": \"%s\",\n", fileContents(path.metadata.lock).data());
    }

    if (fs::exists(path.metadata.notes)) {
        printf("    \"note\": \"%s\"\n", fileContents(path.metadata.notes).data());
    } else {
        printf("    \"note\": \"\"\n");
    }

    printf("}\n");
}

void load() {
    if (!path.holdsData()) {
        fprintf(stderr, "%s", formatMessage("[error]❌ The clipboard you're trying to load from is empty. [help]Try choosing a different source instead.[blank]\n").data());
        exit(EXIT_FAILURE);
    }

    std::vector<std::string> destinations;
    if (!copying.items.empty())
        std::transform(copying.items.begin(), copying.items.end(), std::back_inserter(destinations), [](const auto& item) { return item.string(); });
    else
        destinations.emplace_back(constants.default_clipboard_name);

    if (std::find(destinations.begin(), destinations.end(), clipboard_name) != destinations.end()) {
        stopIndicator();
        fprintf(stderr,
                "%s",
                formatMessage("[error]❌ You can't load a clipboard into itself. [help]Try choosing a different source instead, or choose different destinations.[blank]\n").data());
        exit(EXIT_FAILURE);
    }

    for (const auto& destination_number : destinations) {
        Clipboard destination(destination_number);
        try {
            for (const auto& entry : fs::directory_iterator(destination.data)) {
                fs::remove_all(entry.path());
            }

            fs::copy(path.data, destination.data, fs::copy_options::recursive);

            destination.applyIgnoreRegexes();

            successes.clipboards++;
        } catch (const fs::filesystem_error& e) {
            copying.failedItems.emplace_back(destination_number, e.code());
        }
    }

    stopIndicator();

    if (std::find(destinations.begin(), destinations.end(), constants.default_clipboard_name) != destinations.end()) updateGUIClipboard(true);
}

void swap() {
    if (copying.items.size() > 1) {
        fprintf(stderr,
                formatMessage("[error]❌ You can only swap one clipboard at a time. [help]Try making sure there's only one other clipboard specified, like [bold]%s swap "
                              "5[blank][help] or [bold]%s swap3 0[blank][help].[blank]\n")
                        .data(),
                clipboard_invocation.data(),
                clipboard_invocation.data());
        exit(EXIT_FAILURE);
    }

    std::string destination_name = copying.items.empty() ? std::string(constants.default_clipboard_name) : copying.items.at(0).string();

    if (destination_name == clipboard_name) {
        fprintf(stderr,
                formatMessage("[error]❌ You can't swap a clipboard with itself. [help]Try choosing a different clipboard to swap with, like [bold]%s swap 5[blank][help] or "
                              "[bold]%s swap3 0[blank][help].[blank]\n")
                        .data(),
                clipboard_invocation.data(),
                clipboard_invocation.data());
        exit(EXIT_FAILURE);
    }

    Clipboard destination(destination_name);

    fs::path swapTargetSource(path.data);
    swapTargetSource.replace_extension("swap");

    fs::path swapTargetDestination(destination.data);
    swapTargetDestination.replace_extension("swap");

    try {
        fs::copy(destination.data, swapTargetSource, fs::copy_options::recursive);

        fs::copy(path.data, swapTargetDestination, fs::copy_options::recursive);

        fs::remove_all(path.data);

        fs::remove_all(destination.data);

        fs::rename(swapTargetSource, path.data);

        fs::rename(swapTargetDestination, destination.data);
    } catch (const fs::filesystem_error& e) {
        copying.failedItems.emplace_back(destination_name, e.code());
    }

    stopIndicator();

    fprintf(stderr, formatMessage("[success]✅ Swapped clipboard %s with %s[blank]\n").data(), clipboard_name.data(), destination_name.data());

    if (destination_name == constants.default_clipboard_name) updateGUIClipboard(true);
}

void importClipboards() {
    fs::path importDirectory;
    if (copying.items.empty())
        importDirectory = fs::current_path() / "Exported_Clipboards";
    else
        importDirectory = copying.items.at(0);

    if (!fs::exists(importDirectory)) {
        stopIndicator();
        fprintf(stderr, "%s", formatMessage("[error]❌ The directory you're trying to import from doesn't exist. 💡 [help]Try choosing a different one instead.[blank]\n").data());
        exit(EXIT_FAILURE);
    }

    if (!fs::is_directory(importDirectory)) {
        stopIndicator();
        fprintf(stderr, "%s", formatMessage("[error]❌ The directory you're trying to import from isn't a directory. 💡 [help]Try choosing a different one instead.[blank]\n").data()
        );
        exit(EXIT_FAILURE);
    }

    for (const auto& entry : fs::directory_iterator(importDirectory)) {
        if (!fs::is_directory(entry.path()))
            copying.failedItems.emplace_back(entry.path().filename().string(), std::make_error_code(std::errc::not_a_directory));
        else {
            try {
                auto target = (isPersistent(entry.path().filename().string()) || getenv("CLIPBOARD_ALWAYS_PERSIST") ? global_path.persistent : global_path.temporary)
                              / entry.path().filename();
                if (fs::exists(target)) {
                    using enum CopyPolicy;
                    switch (copying.policy) {
                    case SkipAll:
                        continue;
                    case ReplaceAll:
                        fs::remove_all(target);
                        break;
                    default:
                        stopIndicator();
                        copying.policy = userDecision(entry.path().filename().string());
                        startIndicator();
                        if (copying.policy == ReplaceOnce || copying.policy == ReplaceAll) {
                            fs::copy(entry.path(), target, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
                            successes.clipboards++;
                        }
                        break;
                    }
                } else {
                    fs::copy(entry.path(), target, fs::copy_options::recursive);
                    successes.clipboards++;
                }
            } catch (const fs::filesystem_error& e) {
                copying.failedItems.emplace_back(entry.path().filename().string(), e.code());
            }
        }
    }
}

void exportClipboards() {
    std::vector<std::string> destinations;
    if (!copying.items.empty())
        std::transform(copying.items.begin(), copying.items.end(), std::back_inserter(destinations), [](const auto& item) { return item.string(); });
    else {
        for (const auto& entry : fs::directory_iterator(global_path.temporary))
            destinations.emplace_back(entry.path().filename().string());
        for (const auto& entry : fs::directory_iterator(global_path.persistent))
            destinations.emplace_back(entry.path().filename().string());
    }

    fs::path exportDirectory(fs::current_path() / "Exported_Clipboards");

    try {
        if (fs::exists(exportDirectory)) fs::remove_all(exportDirectory);
        fs::create_directory(exportDirectory);
    } catch (const fs::filesystem_error& e) {
        stopIndicator();
        fprintf(stderr, "%s", formatMessage("[error]❌ CB couldn't create the export directory. 💡 [help]Try checking if you have the right permissions or not.[blank]\n").data());
        exit(EXIT_FAILURE);
    }

    auto exportClipboard = [&](const std::string& name) {
        try {
            Clipboard clipboard(name);
            clipboard.getLock();
            if (clipboard.isUnused()) return;
            fs::copy(clipboard, exportDirectory / name, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            fs::remove(exportDirectory / name / constants.metadata_directory / constants.lock_name);
            clipboard.releaseLock();
            successes.clipboards++;
        } catch (const fs::filesystem_error& e) {
            copying.failedItems.emplace_back(name, e.code());
        }
    };

    for (const auto& name : destinations)
        exportClipboard(name);

    if (destinations.empty() || successes.clipboards == 0) {
        stopIndicator();
        printf("%s", no_clipboard_contents_message().data());
        printf(clipboard_action_prompt().data(), clipboard_invocation.data(), clipboard_invocation.data());
        exit(EXIT_FAILURE);
    }
}

void ignoreRegex() {
    std::vector<std::string> regexes;
    if (io_type == IOType::Pipe)
        regexes.emplace_back(pipedInContent());
    else
        std::transform(copying.items.begin(), copying.items.end(), std::back_inserter(regexes), [](const auto& item) { return item.string(); });

    if (regexes.empty()) {
        if (fs::exists(path.metadata.ignore) && !fs::is_empty(path.metadata.ignore)) {
            std::vector<std::string> ignorePatterns(fileLines(path.metadata.ignore));

            if (is_tty.out) {
                fprintf(stdout, "%s", formatMessage("[info]🔷 Ignore patterns for this clipboard: [help]").data());
                for (const auto& pattern : ignorePatterns) {
                    fprintf(stdout, "%s", pattern.data());
                    if (pattern != ignorePatterns.back()) fprintf(stdout, ", ");
                }
                fprintf(stdout, "%s", formatMessage("[blank]\n").data());
            } else {
                for (const auto& pattern : ignorePatterns) {
                    printf("%s", pattern.data());
                    if (pattern != ignorePatterns.back()) printf(", ");
                }
            }
        } else {
            fprintf(stderr, "%s", formatMessage("[info]🔷 There are no ignore patterns for this clipboard.[blank]\n").data());
        }
        return;
    }

    if (regexes.size() == 1 && (regexes.at(0) == "" || regexes.at(0) == "\n")) {
        fs::remove(path.metadata.ignore);
        if (output_silent) return;
        stopIndicator();
        fprintf(stderr, "%s", formatMessage("[success]✅ Removed ignore patterns\n").data());
        exit(EXIT_SUCCESS);
    }

    std::string writeToFileContent;
    for (const auto& pattern : regexes)
        writeToFileContent += pattern + "\n";

    writeToFile(path.metadata.ignore, writeToFileContent);

    stopIndicator();
    fprintf(stderr, "%s", formatMessage("[success]✅ Saved ignore patterns [bold]").data());
    for (const auto& pattern : regexes) {
        fprintf(stderr, "%s", pattern.data());
        if (pattern != regexes.back()) fprintf(stderr, ", ");
    }
    fprintf(stderr, "%s", formatMessage("[blank]\n").data());
    exit(EXIT_SUCCESS);
}

} // namespace PerformAction