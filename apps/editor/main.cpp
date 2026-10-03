#include "view.hpp"
#include "worker.hpp"

#include <aaf/rpc/server.hpp>

#include <portable-file-dialogs.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aaf::embedded
{
auto indexHtml() -> std::string_view;
}

namespace
{

using aaf::rpc::Json;

constexpr const char* kUsage = "usage: aafedit [file.aaf] [--debug] [--smoke-test file.aaf] [--version]\n";

auto fileFilter(bool anyFile) -> std::vector<std::string>
{
    if (anyFile)
    {
        return { "All files", "*" };
    }
    return { "AAF files", "*.aaf *.AAF", "All files", "*" };
}

void printLine(std::FILE* stream, const std::string& text)
{
    (void) std::fputs(text.c_str(), stream);
    (void) std::fputs("\n", stream);
}

struct Options
{
    std::string file;
    std::string smokeFile;
    bool debug = false;
    bool version = false;
};

auto parse(std::span<char*> argv) -> std::optional<Options>
{
    Options options;
    for (std::size_t i = 1; i < argv.size(); ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--debug")
        {
            options.debug = true;
        }
        else if (arg == "--version")
        {
            options.version = true;
        }
        else if (arg == "--smoke-test" && i + 1 < argv.size())
        {
            options.smokeFile = argv[++i];
        }
        else if (!arg.starts_with("-") && options.file.empty())
        {
            options.file = arg;
        }
        else
        {
            return std::nullopt;
        }
    }
    return options;
}

auto hostCommand(aafedit::View& view, const std::string& command, const Json& options, int& exitCode, bool smoke) -> Json
{
    const bool data = options.value("data", false);
    if (command == "openDialog")
    {
        const auto chosen = pfd::open_file(data ? "Choose a file" : "Open AAF file", "", fileFilter(data)).result();
        return chosen.empty() ? Json(nullptr) : Json(chosen.front());
    }
    if (command == "saveDialog")
    {
        const auto suggested = options.value("suggested", std::string{});
        const auto chosen = pfd::save_file(data ? "Save file" : "Save AAF file", suggested, fileFilter(data), pfd::opt::none).result();
        return chosen.empty() ? Json(nullptr) : Json(chosen);
    }
    if (command == "simulateDrop" && smoke)
    {
        const auto path = std::filesystem::absolute(std::filesystem::path(options.value("path", std::string{}))).u8string();
        view.drop(std::string(path.begin(), path.end()));
        return nullptr;
    }
    if (command == "setTitle")
    {
        view.setTitle(options.value("title", std::string("AAF Editor")));
        return nullptr;
    }
    if (command == "quit")
    {
        exitCode = options.value("code", 0);
        if (const auto message = options.value("message", std::string{}); !message.empty())
        {
            printLine(exitCode == 0 ? stdout : stderr, message);
        }
        view.terminate();
        return nullptr;
    }
    return { { "error", "unknown host command" } };
}

auto run(std::span<char*> argv) -> int
{
    const auto options = parse(argv);
    if (!options)
    {
        (void) std::fputs(kUsage, stderr);
        return 2;
    }
    if (options->version)
    {
        printLine(stdout, std::string("aafedit ") + AAF_VERSION);
        return 0;
    }
    std::atomic<bool> closing = false;
    aafedit::View view(options->debug);
    view.setTitle("AAF Editor");
    view.setSize(1400, 900);

    aafedit::Worker worker;
    aaf::rpc::Server server;
    int exitCode = 0;
    std::atomic<bool> smokeFinished = false;

    server.setEventSink([&view, &closing](const std::string& method, const Json& params) -> void {
        const auto js = "window.__aafEvent && window.__aafEvent(" + Json{ { "method", method }, { "params", params } }.dump() + ")";
        view.dispatch([&view, &closing, js] -> void {
            if (!closing)
            {
                view.eval(js);
            }
        });
    });

    view.bind(
        "aafRpc",
        [&](const std::string& id, const std::string& request) -> void {
            if (closing)
            {
                return;
            }
            worker.post([&, id, request] -> void {
                std::string response;
                try
                {
                    const auto args = Json::parse(request);
                    response = server.handle(args.at(0).get<std::string>());
                } catch (const std::exception& e)
                {
                    response = Json{ { "jsonrpc", "2.0" }, { "id", nullptr }, { "error", { { "code", -32603 }, { "message", e.what() } } } }.dump();
                }
                if (!closing)
                {
                    view.resolve(id, response);
                }
            });
        }
    );

    view.bind(
        "aafHost",
        [&](const std::string& id, const std::string& request) -> void {
            Json result;
            try
            {
                const auto args = Json::parse(request);
                const auto command = args.empty() ? std::string{} : args.at(0).get<std::string>();
                const Json commandOptions = args.size() > 1 && args.at(1).is_object() ? args.at(1) : Json::object();
                if (command == "quit")
                {
                    smokeFinished = true;
                    closing = true;
                }
                result = hostCommand(view, command, commandOptions, exitCode, !options->smokeFile.empty());
            } catch (const std::exception& e)
            {
                result = { { "error", e.what() } };
            }
            view.resolve(id, result.dump());
        }
    );

    view.onFileDrop([&view](const std::string& path) -> void {
        try
        {
            view.eval("window.__aafOpenFile && window.__aafOpenFile(" + Json(path).dump() + ")");
        } catch (const std::exception& e)
        {
            printLine(stderr, std::string("aafedit: cannot open the dropped file: ") + e.what());
        }
    });

    if (!options->file.empty())
    {
        const auto path = std::filesystem::absolute(options->file).string();
        worker.post([&server, path] -> void {
            if (auto opened = server.call("doc.open", { { "path", path } }); !opened)
            {
                printLine(stderr, "aafedit: " + aaf::to_string(opened.error()));
            }
        });
    }

    std::jthread watchdog;
    if (!options->smokeFile.empty())
    {
        view.init("window.__aafSmoke = " + Json{ { "file", std::filesystem::absolute(options->smokeFile).string() } }.dump() + ";");
        watchdog = std::jthread([&view, &smokeFinished, &exitCode](const std::stop_token& stop) -> void {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
            {
                if (smokeFinished)
                {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!smokeFinished && !stop.stop_requested())
            {
                (void) std::fputs("aafedit: smoke test timed out\n", stderr);
                exitCode = 3;
                view.dispatch([&view] -> void { view.terminate(); });
            }
        });
    }

    view.setHtml(std::string(aaf::embedded::indexHtml()));
    view.run();
    // Calls can still arrive while the window is torn down; ignore them, and finish the worker while the server and
    // the view it reports to are both alive.
    closing = true;
    worker.stop();
    if (watchdog.joinable())
    {
        watchdog.request_stop();
        watchdog.join();
    }
    return exitCode;
}

}

auto main(int argc, char** argv) -> int
{
    try
    {
        return run(std::span(argv, static_cast<std::size_t>(argc)));
    } catch (const std::exception& e)
    {
        (void) std::fputs("aafedit: ", stderr);
        (void) std::fputs(e.what(), stderr);
        (void) std::fputs("\n", stderr);
        return 1;
    }
}
