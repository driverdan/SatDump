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
         * Both sizes the archive claims for the entry are checked before
         * anything is allocated: the compressed data must fit in the archive,
         * and the uncompressed size must be one deflate can produce from it.
         * The allocation is then bounded by about 1032 times the size of the
         * archive, rather than by whatever the archive claims.
         *
         * @param zip an initialized miniz reader
         * @param index index of the entry in the archive
         * @param out the extracted data. Left empty on failure
         * @return true on success, false if the entry is invalid, implausibly
         * large or fails to extract
         */
        inline bool extractZipEntry(mz_zip_archive *zip, int index, std::vector<uint8_t> &out)
        {
            std::vector<uint8_t>().swap(out);

            mz_zip_archive_file_stat stat;
            if (index < 0 || !mz_zip_reader_file_stat(zip, index, &stat))
                return false;

            // The compressed size is claimed by the archive too, and miniz only
            // checks it at init for entries without ZIP64 sizes: it must fit
            // in the archive, after the entry's local header
            const uint64_t archive_size = mz_zip_get_archive_size(zip);
            if (stat.m_local_header_ofs > archive_size || stat.m_comp_size > archive_size - stat.m_local_header_ofs)
                return false;

            if (stat.m_uncomp_size == 0)
                return true;

            // Data cannot come out of nothing, and miniz reports success
            // without writing anything for such an entry
            if (stat.m_comp_size == 0)
                return false;

            // Deflate cannot expand data more than 1032 times, so a larger claim
            // is corrupt (divided, as a ZIP64 compressed size times 1032 could wrap)
            if (stat.m_uncomp_size / 1032 > stat.m_comp_size)
                return false;
            if (stat.m_uncomp_size > SIZE_MAX)
                return false;

            try
            {
                out.resize((size_t)stat.m_uncomp_size);
            }
            catch (std::exception &)
            {
                std::vector<uint8_t>().swap(out);
                return false;
            }

            if (!mz_zip_reader_extract_to_mem(zip, index, out.data(), out.size(), 0))
            {
                std::vector<uint8_t>().swap(out);
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