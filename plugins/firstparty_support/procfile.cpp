#include "procfile.h"
#include "core/exception.h"
#include "logger.h"
#include "product_info.h"
#include "type.h"
#include "utils/file/file_iterators.h"
#include "utils/file/folder_file_iterators.h"
#include "utils/file/zip_file_iterators.h"
#include <H5Cpp.h>
#include <exception>
#include <memory>

namespace satdump
{
    namespace firstparty
    {
        FirstPartyProductInfo identifyFirstPartyProductFile(std::string fpath)
        {
            // Identify the product to process. If a single file, just work off that
            // and figure out if it's multi-file, if it's a zip just identify the first
            // one we can.

            FirstPartyProductInfo toproc_info;

            bool is_zip = std::filesystem::path(fpath).extension() == ".zip";
            bool is_file = std::filesystem::is_regular_file(fpath);

            if (is_zip)
            {
                // Test if the ZIP itself is a product
                std::shared_ptr<satdump::utils::FilesIteratorItem> _f = std::make_shared<satdump::utils::FolderFileIteratorItem>(fpath);
                toproc_info = parseFirstPartyInfo(_f);

                if (toproc_info.type == PRODUCT_NONE)
                {
                    satdump::utils::ZipFilesIterator fit(fpath);
                    std::shared_ptr<satdump::utils::FilesIteratorItem> f;

                    while (fit.getNext(f))
                    {
                        if (f)
                        {
                            toproc_info = parseFirstPartyInfo(f);
                            if (toproc_info.type != PRODUCT_NONE)
                                break;
                        }
                    }
                }
            }
            else if (is_file)
            {
                std::shared_ptr<satdump::utils::FilesIteratorItem> f = std::make_shared<satdump::utils::FolderFileIteratorItem>(fpath);
                toproc_info = parseFirstPartyInfo(f);
            }
            else
            {
                logger->error("Not a file or ZIP! => %s", fpath.c_str());
            }

            return toproc_info;
        }

        std::shared_ptr<products::Product> processFirstPartyProductFile(FirstPartyProductInfo toproc_info, std::string fpath)
        {
            // Check it is valid & setup processor

            if (toproc_info.type == PRODUCT_NONE)
                throw satdump_exception("No product present in file " + fpath + "!");

            logger->debug("Processing product : %s", toproc_info.name.c_str());

            auto processor = getProcessorForProduct(toproc_info);

            // Run all required files through.
            bool is_zip = std::filesystem::path(fpath).extension() == ".zip";

            if (toproc_info.group_id.size() || is_zip) // If we have a group, we need to iterate through all of them (also do so if we got a single one in a file)
            {
                logger->trace("Group : %s", toproc_info.group_id.c_str());

                std::shared_ptr<satdump::utils::FilesIterator> fit;
                if (is_zip)
                    fit = std::make_unique<satdump::utils::ZipFilesIterator>(fpath);
                else
                    fit = std::make_unique<satdump::utils::FolderFilesIterator>(std::filesystem::path(fpath).parent_path().string());

                std::shared_ptr<satdump::utils::FilesIteratorItem> f;
                while (fit->getNext(f))
                {
                    if (!f)
                        continue;

                    if (toproc_info.group_id == parseFirstPartyInfo(f).group_id)
                    {
                        logger->trace(" - %s", f->name.c_str());

                        try
                        {
                            processor->ingestFile(f->getPayload());
                        }
                        catch (std::exception &e)
                        {
                            logger->error("Error processing file! %s", e.what());
                        }
                        catch (H5::Exception &e)
                        {
                            logger->error("Error processing file! %s", e.getCDetailMsg());
                        }
                    }
                }
            }
            else
            {
                std::unique_ptr<satdump::utils::FilesIteratorItem> f = std::make_unique<satdump::utils::FolderFileIteratorItem>(fpath);
                logger->trace(" - %s", f->name.c_str());
                processor->ingestFile(f->getPayload());
            }

            // Finally, return product
            auto product = processor->getProduct();
            // product->save("/tmp/satdump_firstparty/test");
            return product;
        }
    } // namespace firstparty
} // namespace satdump