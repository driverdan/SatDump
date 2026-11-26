#pragma once

#include "core/exception.h"
#include "file_iterators.h"
#include "libs/miniz/miniz.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace satdump
{
    namespace utils
    {
        /**
         * @brief Extract a zip entry into a vector.
         *
         * The uncompressed size the archive claims is checked against what
         * its compressed size can plausibly expand to before anything is
         * allocated, so that a corrupt or malicious archive cannot request a
         * huge allocation.
         *
         * @param zip an initialized miniz reader
         * @param index index of the entry in the archive
         * @param out the extracted data. Left empty on failure
         * @return true on success, false if the entry is invalid, implausibly
         * large or fails to extract
         */
        inline bool extractZipEntry(mz_zip_archive *zip, int index, std::vector<uint8_t> &out)
        {
            out.clear();

            mz_zip_archive_file_stat stat;
            if (index < 0 || !mz_zip_reader_file_stat(zip, index, &stat))
                return false;

            // Deflate cannot expand data more than 1032 times, so a larger claim
            // is corrupt (divided, as a ZIP64 compressed size times 1032 could wrap)
            if (stat.m_uncomp_size / 1032 > stat.m_comp_size)
                return false;
            if (stat.m_uncomp_size > SIZE_MAX)
                return false;

            if (stat.m_uncomp_size == 0)
                return true;

            try
            {
                out.resize((size_t)stat.m_uncomp_size);
            }
            catch (std::exception &)
            {
                out.clear();
                return false;
            }

            if (!mz_zip_reader_extract_to_mem(zip, index, out.data(), out.size(), 0))
            {
                out.clear();
                out.shrink_to_fit();
                return false;
            }

            return true;
        }

        class ZipFileIteratorItem : public FilesIteratorItem
        {
        private:
            const mz_zip_archive *zip;
            const int num;

        public:
            ZipFileIteratorItem(mz_zip_archive *zip, int num, std::string name) : FilesIteratorItem(name), zip(zip), num(num) {}
            ~ZipFileIteratorItem() {}

            std::vector<uint8_t> getPayload()
            {
                std::vector<uint8_t> vec;
                extractZipEntry((mz_zip_archive *)zip, num, vec);
                return vec;
            }
        };

        class ZipFilesIterator : public FilesIterator
        {
        private:
            mz_zip_archive zip{};
            int numfiles;
            int file_index;

        public:
            ZipFilesIterator(std::string zipfile)
            {
                if (!mz_zip_reader_init_file(&zip, zipfile.c_str(), 0))
                    throw satdump_exception("Invalid zip file! " + zipfile);
                numfiles = mz_zip_reader_get_num_files(&zip);
                file_index = 0;
            }

            ~ZipFilesIterator() { mz_zip_reader_end(&zip); }

            bool getNext(std::shared_ptr<FilesIteratorItem> &v)
            {
                bool vv = file_index < numfiles;

                if (vv)
                {
                    v.reset();

                    if (mz_zip_reader_is_file_supported(&zip, file_index))
                    {
                        char filename[2000]; // TODOREWORK?
                        if (mz_zip_reader_get_filename(&zip, file_index, filename, 2000))
                            v = std::make_unique<ZipFileIteratorItem>(&zip, file_index, std::filesystem::path(filename).stem().string() + std::filesystem::path(filename).extension().string());
                    }

                    file_index++;
                }

                return vv;
            }

            void reset() { file_index = 0; }
        };
    } // namespace utils
} // namespace satdump