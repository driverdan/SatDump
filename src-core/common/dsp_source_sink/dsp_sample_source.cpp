#define SATDUMP_DLL_EXPORT 1

#include "dsp_sample_source.h"
#include "core/plugin.h"
#include "file_source.h"
#include "logger.h"

namespace dsp
{
    SATDUMP_DLL std::map<std::string, RegisteredSource> dsp_sources_registry;

    std::vector<SourceDescriptor> getAllAvailableSources(bool remote)
    {
        std::vector<SourceDescriptor> all_sources;

        for (std::pair<std::string, RegisteredSource> source : dsp_sources_registry)
        {
            if (remote)
                if (source.first == "remote")
                    continue;
            std::vector<SourceDescriptor> devices = getAvailableSourcesOfType(source.first);
            all_sources.insert(all_sources.end(), devices.begin(), devices.end());
        }

        return all_sources;
    }

    std::vector<SourceDescriptor> getAvailableSourcesOfType(std::string type)
    {
        auto source = dsp_sources_registry.find(type);
        if (source == dsp_sources_registry.end())
            return {};

        try
        {
            return source->second.getSources();
        }
        catch (std::exception &e)
        {
            logger->error("Error trying to list " + type + " sources! %s", e.what());
            return {};
        }
    }

    std::shared_ptr<DSPSampleSource> getSourceFromDescriptor(SourceDescriptor descriptor)
    {
        for (std::pair<std::string, RegisteredSource> source : dsp_sources_registry)
            if (descriptor.source_type == source.first)
                return source.second.getInstance(descriptor);
        throw satdump_exception("Could not find a handler for device " + descriptor.name);
    }

    void registerAllSources()
    {
        dsp_sources_registry.insert({FileSource::getID(), {FileSource::getInstance, FileSource::getAvailableSources}});

        // Plugin Sources
        satdump::eventBus->fire_event<RegisterDSPSampleSourcesEvent>({dsp_sources_registry});
    }
}