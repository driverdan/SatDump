#pragma once

// Shared helpers for the pipeline/ and handlers/ tests. Everything lives in
// the pipeline_test namespace and is inline, so including this header
// from several translation units is fine.

#include "core/config.h"
#include "core/plugin.h"
#include "helpers/file_io.h"
#include "helpers/guards.h"
#include "helpers/satdump_guards.h"
#include "helpers/temp_files.h"
#include "helpers/wait.h"
#include "init.h"
#include "pipeline/module.h"
#include "pipeline/pipeline.h"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Internal (non-header) symbols of pipeline.cpp that are exported from the core library
namespace satdump
{
    namespace pipeline
    {
        void loadPipeline(std::string filepath);
        void parsePipelines();
#ifndef _MSC_VER
        // A data symbol: not exported from the Windows DLL, so not available with MSVC
        extern std::string user_cfg_path;
#endif
    } // namespace pipeline
} // namespace satdump

namespace pipeline_test
{
    using test_helpers::ConfigGuard;
    using test_helpers::CwdGuard;
    using test_helpers::EventBusGuard;
    using test_helpers::readBytes;
    using test_helpers::readFile;
    using test_helpers::TempDir;
    using test_helpers::writeBytes;
    using test_helpers::writeFile;

    // Saves the module registry, optionally clears it, and restores it afterwards
    struct ModuleRegistryGuard
    {
        std::vector<satdump::pipeline::ModuleEntry> saved;

        explicit ModuleRegistryGuard(bool clear = true) : saved(satdump::pipeline::modules_registry)
        {
            if (clear)
                satdump::pipeline::modules_registry.clear();
        }

        ~ModuleRegistryGuard() { satdump::pipeline::modules_registry = saved; }
    };

    // Saves all global pipeline state, starts from an empty state, and restores it afterwards.
    // With MSVC user_cfg_path is not restored, as it is not exported from the DLL
    // (harmless: loadPipelines() always sets it before it is used).
    struct PipelinesStateGuard
    {
        std::vector<satdump::pipeline::Pipeline> saved_pipelines;
        nlohmann::ordered_json saved_json, saved_system_json;
#ifndef _MSC_VER
        std::string saved_user_cfg_path;
#endif

        PipelinesStateGuard() : saved_pipelines(satdump::pipeline::pipelines), saved_json(satdump::pipeline::pipelines_json), saved_system_json(satdump::pipeline::pipelines_system_json)
        {
#ifndef _MSC_VER
            saved_user_cfg_path = satdump::pipeline::user_cfg_path;
#endif
            satdump::pipeline::pipelines.clear();
            satdump::pipeline::pipelines_json = nlohmann::ordered_json::object();
            satdump::pipeline::pipelines_system_json = nlohmann::ordered_json::object();
        }

        ~PipelinesStateGuard()
        {
            satdump::pipeline::pipelines = saved_pipelines;
            satdump::pipeline::pipelines_json = saved_json;
            satdump::pipeline::pipelines_system_json = saved_system_json;
#ifndef _MSC_VER
            satdump::pipeline::user_cfg_path = saved_user_cfg_path;
#endif
        }
    };

    struct UserPathGuard
    {
        std::string saved;
        explicit UserPathGuard(const std::string &p) : saved(satdump::user_path) { satdump::user_path = p; }
        ~UserPathGuard() { satdump::user_path = saved; }
    };

    ///////////////////////////////////////////////////////////
    // Recording file-to-file module used to exercise Pipeline::run
    ///////////////////////////////////////////////////////////

    struct ModuleCall
    {
        std::string event; // "construct" or "process"
        std::string id;
        std::string input;
        std::string hint;
        nlohmann::json params;
        int input_type = -1;
        int output_type = -1;
    };

    inline std::mutex &callsMutex()
    {
        static std::mutex m;
        return m;
    }

    inline std::vector<ModuleCall> &moduleCalls()
    {
        static std::vector<ModuleCall> c;
        return c;
    }

    inline void recordCall(const ModuleCall &c)
    {
        std::lock_guard<std::mutex> l(callsMutex());
        moduleCalls().push_back(c);
    }

    inline std::vector<ModuleCall> callsOf(const std::string &event)
    {
        std::lock_guard<std::mutex> l(callsMutex());
        std::vector<ModuleCall> out;
        for (auto &c : moduleCalls())
            if (c.event == event)
                out.push_back(c);
        return out;
    }

    struct CallsGuard
    {
        CallsGuard()
        {
            std::lock_guard<std::mutex> l(callsMutex());
            moduleCalls().clear();
        }
        ~CallsGuard()
        {
            std::lock_guard<std::mutex> l(callsMutex());
            moduleCalls().clear();
        }
    };

    // Copies its input file to <hint>_<id>.bin and appends "[<id>]".
    // With stream_out it can also push its data into output_fifo, and with
    // stream_in it can read from input_fifo until the reader is stopped.
    class RecordingModule : public satdump::pipeline::ProcessingModule
    {
    private:
        std::string id;
        bool stream_in, stream_out;

    public:
        RecordingModule(std::string id, std::string input_file, std::string output_file_hint, nlohmann::json parameters, bool stream_in, bool stream_out)
            : ProcessingModule(input_file, output_file_hint, parameters), id(id), stream_in(stream_in), stream_out(stream_out)
        {
            recordCall({"construct", id, input_file, output_file_hint, parameters});
        }

        std::vector<satdump::pipeline::ModuleDataType> getInputTypes()
        {
            if (stream_in)
                return {satdump::pipeline::DATA_FILE, satdump::pipeline::DATA_STREAM};
            return {satdump::pipeline::DATA_FILE};
        }

        std::vector<satdump::pipeline::ModuleDataType> getOutputTypes()
        {
            if (stream_out)
                return {satdump::pipeline::DATA_FILE, satdump::pipeline::DATA_STREAM};
            return {satdump::pipeline::DATA_FILE};
        }

        void process()
        {
            recordCall({"process", id, d_input_file, d_output_file_hint, d_parameters, (int)input_data_type, (int)output_data_type});

            std::string data;
            if (input_data_type == satdump::pipeline::DATA_FILE)
            {
                data = readFile(d_input_file);
            }
            else
            {
                uint8_t b;
                while (input_fifo->read(&b, 1) == 1)
                    data.push_back((char)b);
            }

            data += "[" + id + "]";

            if (output_data_type == satdump::pipeline::DATA_STREAM)
            {
                output_fifo->write((uint8_t *)data.data(), data.size());
            }
            else
            {
                d_output_file = d_output_file_hint + "_" + id + ".bin";
                writeFile(d_output_file, data);
            }
        }

        void drawUI(bool) {}
        std::string getIDM() { return id; }
    };

    inline satdump::pipeline::ModuleEntry recordingEntry(const std::string &id, bool stream_in = false, bool stream_out = false)
    {
        return satdump::pipeline::ModuleEntry{id, nlohmann::json::object(),
                                              [id, stream_in, stream_out](std::string in, std::string hint, nlohmann::json params) -> std::shared_ptr<satdump::pipeline::ProcessingModule>
                                              { return std::make_shared<RecordingModule>(id, in, hint, params, stream_in, stream_out); }};
    }

    ///////////////////////////////////////////////////////////
    // Streaming modules that can fail, to exercise the module threads
    ///////////////////////////////////////////////////////////

    // Counts what the failing modules did, shared by every instance of the same id
    struct StreamStats
    {
        std::atomic<long> written{0};
        std::atomic<long> read{0};
        std::atomic<bool> done{false};
    };

    inline std::shared_ptr<StreamStats> streamStats(const std::string &id)
    {
        static std::mutex m;
        static std::map<std::string, std::shared_ptr<StreamStats>> s;
        std::lock_guard<std::mutex> l(m);
        if (!s.count(id) || s[id] == nullptr)
            s[id] = std::make_shared<StreamStats>();
        return s[id];
    }

    inline void resetStreamStats(const std::string &id)
    {
        auto st = streamStats(id);
        st->written = 0;
        st->read = 0;
        st->done = false;
    }

    // Writes `bytes` bytes to its output FIFO (or file), then throws "<id> broke" if fail is set
    class FailingSourceModule : public satdump::pipeline::ProcessingModule
    {
    private:
        std::string id;
        long bytes;
        bool fail;

    public:
        FailingSourceModule(std::string id, std::string input_file, std::string output_file_hint, nlohmann::json parameters, long bytes, bool fail)
            : ProcessingModule(input_file, output_file_hint, parameters), id(id), bytes(bytes), fail(fail)
        {
        }

        std::vector<satdump::pipeline::ModuleDataType> getInputTypes() { return {satdump::pipeline::DATA_FILE}; }
        std::vector<satdump::pipeline::ModuleDataType> getOutputTypes() { return {satdump::pipeline::DATA_FILE, satdump::pipeline::DATA_STREAM}; }

        void process()
        {
            recordCall({"process", id, d_input_file, d_output_file_hint, d_parameters, (int)input_data_type, (int)output_data_type});
            auto st = streamStats(id);
            std::vector<uint8_t> chunk(10000, 'x');
            for (long left = bytes; left > 0; left -= chunk.size())
            {
                int n = (int)std::min<long>(left, chunk.size());
                if (output_data_type == satdump::pipeline::DATA_STREAM && output_fifo->write(chunk.data(), n) < 0)
                    break;
                st->written += n;
            }
            st->done = true;
            if (fail)
                throw std::runtime_error(id + " broke");
        }

        void drawUI(bool) {}
        std::string getIDM() { return id; }
    };

    // Reads its input FIFO until it is stopped, and writes the number of bytes read to
    // <hint>_<id>.bin. With fail_after >= 0, throws "<id> broke" once it has read that many bytes.
    class FailingSinkModule : public satdump::pipeline::ProcessingModule
    {
    private:
        std::string id;
        long fail_after;

    public:
        FailingSinkModule(std::string id, std::string input_file, std::string output_file_hint, nlohmann::json parameters, long fail_after)
            : ProcessingModule(input_file, output_file_hint, parameters), id(id), fail_after(fail_after)
        {
        }

        std::vector<satdump::pipeline::ModuleDataType> getInputTypes() { return {satdump::pipeline::DATA_FILE, satdump::pipeline::DATA_STREAM}; }
        std::vector<satdump::pipeline::ModuleDataType> getOutputTypes() { return {satdump::pipeline::DATA_FILE}; }

        void process()
        {
            recordCall({"process", id, d_input_file, d_output_file_hint, d_parameters, (int)input_data_type, (int)output_data_type});
            auto st = streamStats(id);
            uint8_t b;
            while (true)
            {
                if (fail_after >= 0 && st->read >= fail_after)
                    throw std::runtime_error(id + " broke");
                if (input_fifo->read(&b, 1) != 1)
                    break;
                st->read++;
            }
            d_output_file = d_output_file_hint + "_" + id + ".bin";
            writeFile(d_output_file, std::to_string(st->read.load()));
            st->done = true;
        }

        void drawUI(bool) {}
        std::string getIDM() { return id; }
    };

    inline satdump::pipeline::ModuleEntry failingSourceEntry(const std::string &id, long bytes, bool fail)
    {
        resetStreamStats(id);
        return satdump::pipeline::ModuleEntry{id, nlohmann::json::object(),
                                              [id, bytes, fail](std::string in, std::string hint, nlohmann::json params) -> std::shared_ptr<satdump::pipeline::ProcessingModule>
                                              { return std::make_shared<FailingSourceModule>(id, in, hint, params, bytes, fail); }};
    }

    inline satdump::pipeline::ModuleEntry failingSinkEntry(const std::string &id, long fail_after)
    {
        resetStreamStats(id);
        return satdump::pipeline::ModuleEntry{id, nlohmann::json::object(),
                                              [id, fail_after](std::string in, std::string hint, nlohmann::json params) -> std::shared_ptr<satdump::pipeline::ProcessingModule>
                                              { return std::make_shared<FailingSinkModule>(id, in, hint, params, fail_after); }};
    }

    // Runs fn on a thread of its own and waits for it, bounded by a timeout, so that a hang
    // fails the test instead of stalling the run. On timeout the thread is left running:
    // everything fn uses must then stay valid, so fn should own it (capture by value).
    // fn must not throw.
    inline bool runBounded(std::function<void()> fn, std::chrono::milliseconds timeout = test_helpers::kTimeout)
    {
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::thread th(
            [fn, done]()
            {
                fn();
                *done = true;
            });
        if (!test_helpers::waitUntil([done]() { return done->load(); }, timeout))
        {
            th.detach();
            return false;
        }
        th.join();
        return true;
    }
} // namespace pipeline_test
