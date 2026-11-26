#include "image/io.h"
#include "libs/miniz/miniz.h"
#include "libs/miniz/miniz_zip.h"
#include "logger.h"
#include "lrit_header.h"
#include "module_goes_lrit_data_decoder.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace goes
{
    namespace hrit
    {
        // Largest file unpacked from an EMWIN ZIP archive. Real products are under 1 MB (the
        // largest seen, GOES imagery, is about 700 kB). An archive claiming more, at a size its
        // data could decompress to, is not unpacked but saved as received, as <name>.zip
        const size_t EMWIN_MAX_FILE_SIZE = 16 * 1024 * 1024;

        std::string getHRITImageFilename(std::tm *timeReadable, std::string sat_name, std::string channel)
        {
            std::string utc_filename = sat_name + "_" + channel + "_" +                                                                                             // Satellite name and channel
                                       std::to_string(timeReadable->tm_year + 1900) +                                                                               // Year yyyy
                                       (timeReadable->tm_mon + 1 > 9 ? std::to_string(timeReadable->tm_mon + 1) : "0" + std::to_string(timeReadable->tm_mon + 1)) + // Month MM
                                       (timeReadable->tm_mday > 9 ? std::to_string(timeReadable->tm_mday) : "0" + std::to_string(timeReadable->tm_mday)) + "T" +    // Day dd
                                       (timeReadable->tm_hour > 9 ? std::to_string(timeReadable->tm_hour) : "0" + std::to_string(timeReadable->tm_hour)) +          // Hour HH
                                       (timeReadable->tm_min > 9 ? std::to_string(timeReadable->tm_min) : "0" + std::to_string(timeReadable->tm_min)) +             // Minutes mm
                                       (timeReadable->tm_sec > 9 ? std::to_string(timeReadable->tm_sec) : "0" + std::to_string(timeReadable->tm_sec)) + "Z";        // Seconds ss
            return utc_filename;
        }

        /**
         * @brief Generic function to save EMWIN files in appropriate directories
         *
         * @param directory Base EMWIN directory
         * @param filename Filename to save as
         * @param buffer Pointer to the data buffer
         * @param size How many bytes to write
         * @param suffix Appended to the name of the file written, which is still sorted by filename
         * @throws std::exception if the file or its directories cannot be written
         */
        void GOESLRITDataDecoderModule::saveEMWINFile(std::string directory, std::string filename, char *buffer, size_t size, std::string suffix)
        {
            if (!std::filesystem::exists(directory + "/EMWIN"))
                std::filesystem::create_directory(directory + "/EMWIN");

            std::string subdirectory;
            std::string extension = filename.size() >= 3 ? filename.substr(filename.size() - 3) : "";
            
            // Only proceed if we are meant to process text files
            if ((extension == "TXT" || extension == "txt") && !write_emwin_text)
                return;

            // All files appear to feature an 8-character ID before the extension, proceed if it looks like it
            // (a "-", the 8-character ID, then a "." and a 3-character extension: 13 characters)
            if (filename.size() >= 13 && !filename.compare(filename.size() - 13, 1, "-"))
            {
                std::string ID = filename.substr(filename.size() - 12, 8);
                // clang-format off

                // Some standard subdirs
                // (compare returning 0 on success is so stupid bruh I hate c++)
                if (
                    !ID.compare(0, 3, "RAD") || !ID.compare(0, 5, "IMGWW") || 
                    !ID.compare(0, 4, "GPHJ") || !ID.compare(0, 3, "MOD") || 
                    !ID.compare(0, 4, "USHZ") || !ID.compare(0, 4, "USHZ") ||
                    !ID.compare(0, 6, "IMGFNT") || !ID.compare(0, 3, "CSA") ||
                    !ID.compare(0, 4, "NPSA") || !ID.compare(0, 3, "NPI"))
                {
                    if (!write_emwin_nws)
                        return;

                    subdirectory = "/NWS/";
                }
                // Sat imagery: MSG
                else if (!ID.compare(0, 3, "IND")) {
                    subdirectory = "/Meteosat/";
                }
                // Sat imagery: Himawari
                else if (!ID.compare(0, 3, "GMS")) {
                    subdirectory = "/Himawari/";
                }
                // Sat imagery: GOES
                else if(
                    
                    !ID.compare(0, 3, "G02") || !ID.compare(0, 3, "G10") || 
                    !ID.compare(0, 3, "G16") || !ID.compare(0, 3, "MOD") || 
                    !ID.compare(0, 8, "IMGSJUPR"))
                {
                    subdirectory = "/GOES/";
                }
                else if (extension == "TXT" || extension == "txt") {
                    subdirectory = "/Text/";
                }
                // Catch-all for crap I don't have from these 
                else {
                    subdirectory = "/Other/";
                }
                // clang-format on
            }
            // We can't use the ID because the filename structure is different, default to "Other"
            else
            {
                subdirectory = "/Other/";
            }

            if (!std::filesystem::exists(directory + "/EMWIN" + subdirectory))
                std::filesystem::create_directory(directory + "/EMWIN" + subdirectory);

            // Write file out. A failure throws, as creating the directories above does
            std::string path = directory + "/EMWIN" + subdirectory + filename + suffix;
            logger->info("Writing file %s...", path.c_str());
            std::ofstream fileo(path, std::ios::binary);
            if (!fileo)
                throw std::runtime_error("cannot open " + path + ": " + strerror(errno));
            fileo.write(buffer, size);
            fileo.close();
            if (!fileo)
                throw std::runtime_error("cannot write " + path + ": " + strerror(errno));
        }

        /**
         * @brief Save a file as received, in its xRIT form
         *
         * @param file The file
         * @param path Directory to save it in
         * @throws std::exception if the file or its directory cannot be written
         */
        void GOESLRITDataDecoderModule::saveLRITFile(satdump::xrit::XRITFile &file, std::string path)
        {
            if (!std::filesystem::exists(path))
                std::filesystem::create_directory(path);

            // If segmented, rename file after segment name
            std::string current_filename = file.filename;
            if (file.hasHeader<SegmentIdentificationHeader>())
            {
                SegmentIdentificationHeader segment_id_header = file.getHeader<SegmentIdentificationHeader>();
                std::ostringstream oss;
                oss << "_" << std::setfill('0') << std::setw(3) << segment_id_header.segment_sequence_number;
                // Before the ".lrit" extension, or at the end of a name too short to have it
                current_filename.insert(current_filename.length() >= 5 ? current_filename.length() - 5 : current_filename.length(), oss.str());
            }

            logger->info("Writing file %s/%s...", path.c_str(), current_filename.c_str());

            // Write file out
            std::ofstream fileo(path + "/" + current_filename, std::ios::binary);
            if (!fileo)
                throw std::runtime_error("cannot open " + path + "/" + current_filename + ": " + strerror(errno));
            fileo.write((char *)file.lrit_data.data(), file.lrit_data.size());
            fileo.close();
            if (!fileo)
                throw std::runtime_error("cannot write " + path + "/" + current_filename + ": " + strerror(errno));
        }

        void GOESLRITDataDecoderModule::processLRITFile(satdump::xrit::XRITFile &file)
        {
            std::string current_filename = file.filename;

            satdump::xrit::PrimaryHeader primary_header = file.getHeader<satdump::xrit::PrimaryHeader>();
            NOAALRITHeader noaa_header = file.getHeader<NOAALRITHeader>();

            // Handle LRIT files with no data
            if (primary_header.total_header_length == file.lrit_data.size())
            {
                logger->warn("Received LRIT header with no body! Saving as .lrit");
                saveLRITFile(file, directory + "/LRIT");
            }

            // Check if this is image data, and if so also write it as an image
            else if (primary_header.file_type_code == 0 && file.hasHeader<satdump::xrit::ImageStructureRecord>())
            {
                if (!write_images)
                    return;

                if (!std::filesystem::exists(directory + "/IMAGES"))
                    std::filesystem::create_directory(directory + "/IMAGES");

                satdump::xrit::ImageStructureRecord image_structure_record = file.getHeader<satdump::xrit::ImageStructureRecord>();

                satdump::xrit::XRITFileInfo finfo = satdump::xrit::identifyXRITFIle(file);

                // Check if this is image data, and if so also write it as an image
                if (finfo.type != satdump::xrit::XRIT_UNKNOWN)
                {
                    std::string processor_name = finfo.satellite_short_name;

                    bool is_l2 = false;
                    if (finfo.channel.find_first_of("0123456789") == std::string::npos)
                    {
                        processor_name = processor_name + "" + finfo.channel;
                        // finfo.region = finfo.channel; TODOREWORK???
                        is_l2 = true;
                    }

                    all_processors_mtx.lock();
                    if (all_processors.count(processor_name) == 0)
                    {
                        auto p = std::make_shared<satdump::xrit::XRITChannelProcessor>();
                        if (is_l2)
                            p->directory = d_output_file_hint.substr(0, d_output_file_hint.rfind('/')) + "/L2";
                        else
                            p->directory = d_output_file_hint.substr(0, d_output_file_hint.rfind('/')) + "/IMAGES";
                        all_processors.emplace(processor_name, p);
                    }
                    all_processors_mtx.unlock();

                    all_processors[processor_name]->push(finfo, file);
                }
                else if (noaa_header.noaa_specific_compression == 5) // Gif?
                {
                    logger->info("Writing file %s/IMAGES/%s.gif...", directory.c_str(), current_filename.c_str());

                    int offset = primary_header.total_header_length;

                    // Write file out
                    std::string path = directory + "/IMAGES/" + current_filename + ".gif";
                    std::ofstream fileo(path, std::ios::binary);
                    if (!fileo)
                        throw std::runtime_error("cannot open " + path + ": " + strerror(errno));
                    fileo.write((char *)&file.lrit_data[offset], file.lrit_data.size() - offset);
                    fileo.close();
                    if (!fileo)
                        throw std::runtime_error("cannot write " + path + ": " + strerror(errno));
                }
                else if (file.lrit_data.size() < (size_t)primary_header.total_header_length + (size_t)image_structure_record.columns_count * image_structure_record.lines_count)
                {
                    // Only uncompressed and Rice compressed images are brought to their size
                    logger->error("File %s is smaller than its %dx%d image, not saving it as an image", current_filename.c_str(), image_structure_record.columns_count,
                                  image_structure_record.lines_count);
                }
                else // Write raw image dats
                {
                    image::Image image(&file.lrit_data[primary_header.total_header_length], 8, image_structure_record.columns_count, image_structure_record.lines_count, 1);
                    image::save_img(image, directory + "/IMAGES/" + current_filename);
                }
            }
            // Check if this EMWIN data
            else if (primary_header.file_type_code == 2 && (noaa_header.product_id == 9 || noaa_header.product_id == 6))
            {
                if (!write_emwin)
                    return;

                std::string clean_filename = current_filename.substr(0, current_filename.size() - 5); // Remove extensions
                if (noaa_header.noaa_specific_compression == 0)                                       // Uncompressed TXT
                {
                    int offset = primary_header.total_header_length;

                    // A file that cannot be written is lost, but the decoder carries on
                    try
                    {
                        saveEMWINFile(directory, clean_filename + ".txt", (char *)&file.lrit_data[offset], file.lrit_data.size() - offset);
                    }
                    catch (const std::exception &e)
                    {
                        logger->error("Could not write EMWIN file %s.txt! %s", clean_filename.c_str(), e.what());
                    }
                }
                else if (noaa_header.noaa_specific_compression == 10) // ZIP Files
                {
                    int offset = primary_header.total_header_length;

                    // Init
                    mz_zip_archive zipFile;
                    MZ_CLEAR_OBJ(zipFile);
                    if (!mz_zip_reader_init_mem(&zipFile, &file.lrit_data[offset], file.lrit_data.size() - offset, MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY))
                    {
                        logger->error("Could not open ZIP data! Discarding...");
                        return;
                    }
                    std::unique_ptr<mz_zip_archive, mz_bool (*)(mz_zip_archive *)> zipGuard(&zipFile, mz_zip_reader_end); // Ended on every path

                    // Read filename. The archive is as received, so it may hold no file, or name it
                    // anything: the name must not leave the EMWIN directory
                    char filenameStr[1000];
                    mz_uint chs = 0;
                    if (mz_zip_reader_get_num_files(&zipFile) > 0)
                        chs = mz_zip_reader_get_filename(&zipFile, 0, filenameStr, 1000);
                    std::string filename(filenameStr, chs > 0 ? chs - 1 : 0);
                    std::replace(filename.begin(), filename.end(), '/', '_');  // Safety
                    std::replace(filename.begin(), filename.end(), '\\', '_'); // Safety
                    std::replace(filename.begin(), filename.end(), '\0', '_');  // Safety
                    if (filename.empty() || filename == "." || filename == "..")
                    {
                        logger->error("ZIP data holds no file! Discarding...");
                        return;
                    }

                    mz_zip_archive_file_stat stat;
                    if (!mz_zip_reader_file_stat(&zipFile, 0, &stat))
                    {
                        logger->error("Could not read ZIP file entry! Discarding...");
                        return;
                    }

                    // The size the archive claims is as received too, and is what the output buffer
                    // is allocated for: check it first. Deflate cannot expand data more than 1032 times
                    // (divided, as a ZIP64 compressed size times 1032 could wrap), so more is corrupt
                    if (stat.m_uncomp_size / 1032 > stat.m_comp_size)
                    {
                        logger->error("ZIP entry %s claims %llu bytes from %llu compressed bytes, more than deflate can produce! Discarding...", filename.c_str(),
                                      (unsigned long long)stat.m_uncomp_size, (unsigned long long)stat.m_comp_size);
                        return;
                    }

                    // A plausible size over the limit is not unpacked: the archive is saved as received
                    if (stat.m_uncomp_size > EMWIN_MAX_FILE_SIZE)
                    {
                        logger->warn("ZIP entry %s claims %llu bytes, over the %llu byte limit for EMWIN files! Saving the archive as received...", filename.c_str(),
                                     (unsigned long long)stat.m_uncomp_size, (unsigned long long)EMWIN_MAX_FILE_SIZE);
                        try
                        {
                            saveEMWINFile(directory, filename, (char *)&file.lrit_data[offset], file.lrit_data.size() - offset, ".zip");
                        }
                        catch (const std::exception &e)
                        {
                            logger->error("Could not write EMWIN file %s.zip! %s", filename.c_str(), e.what());
                        }
                        return;
                    }

                    // Decompress into a buffer of the size checked above, rather than letting
                    // mz_zip_reader_extract_to_heap() allocate one
                    std::vector<uint8_t> outBuffer(stat.m_uncomp_size);
                    if (!mz_zip_reader_extract_to_mem(&zipFile, 0, outBuffer.data(), outBuffer.size(), 0))
                    {
                        logger->error("Could not decompress ZIP data! Discarding...");
                        return;
                    }

                    // Write out. A file that cannot be written is lost, but the decoder carries on
                    try
                    {
                        saveEMWINFile(directory, filename, (char *)outBuffer.data(), outBuffer.size());
                    }
                    catch (const std::exception &e)
                    {
                        logger->error("Could not write EMWIN file %s! %s", filename.c_str(), e.what());
                    }
                }
            }
            // Check if this is message data. If we slipped to here we know it's not EMWIN
            else if (write_messages && (primary_header.file_type_code == 1 || primary_header.file_type_code == 2))
            {
                std::string clean_filename = current_filename.substr(0, current_filename.size() - 5); // Remove extensions

                if (!std::filesystem::exists(directory + "/Admin Messages"))
                    std::filesystem::create_directory(directory + "/Admin Messages");

                logger->info("Writing file %s/Admin Messages/%s.txt...", directory.c_str(), clean_filename.c_str());

                int offset = primary_header.total_header_length;

                // Write file out
                std::string path = directory + "/Admin Messages/" + clean_filename + ".txt";
                std::ofstream fileo(path, std::ios::binary);
                if (!fileo)
                    throw std::runtime_error("cannot open " + path + ": " + strerror(errno));
                fileo.write((char *)&file.lrit_data[offset], file.lrit_data.size() - offset);
                fileo.close();
                if (!fileo)
                    throw std::runtime_error("cannot write " + path + ": " + strerror(errno));
            }
            // Check if this is DCS data
            else if (primary_header.file_type_code == 130)
            {
                int offset = primary_header.total_header_length;
                // A failure here must not lose the copy in DCS_LRIT
                try
                {
                    if (parse_dcs && (file.vcid != 32 || !processDCS(&file.lrit_data.data()[offset], file.lrit_data.size() - offset)))
                        saveLRITFile(file, directory + "/DCS/Unknown");
                }
                catch (const std::exception &e)
                {
                    logger->error("Could not process DCS file %s! %s", current_filename.c_str(), e.what());
                }
                if (write_dcs)
                    saveLRITFile(file, directory + "/DCS_LRIT");
            }
            // Otherwise, write as generic, unknown stuff. This should not happen
            // Do not write if already saving LRIT data
            else if (write_unknown && !write_lrit)
                saveLRITFile(file, directory + "/LRIT");
        }
    } // namespace hrit
} // namespace goes
