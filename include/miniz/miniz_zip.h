#pragma once
#include "miniz_common.h"

/* ------------------- ZIP archive reading/writing */

#ifndef MINIZ_NO_ARCHIVE_APIS

#ifdef __cplusplus
extern "C"
{
#endif

    enum
    {
        /* Note: These enums can be reduced as needed to save memory or stack space - they are pretty conservative. */
        MZ_ZIP_MAX_IO_BUF_SIZE = 64 * 1024,
        MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE = 512,
        MZ_ZIP_MAX_ARCHIVE_FILE_COMMENT_SIZE = 512
    };

    typedef struct
    {
        /* Central directory file index. */
        mz_uint32 m_file_index;

        /* Byte offset of this entry in the archive's central directory. Note we currently only support up to UINT_MAX or less bytes in the central dir. */
        mz_uint64 m_central_dir_ofs;

        /* These fields are copied directly from the zip's central dir. */
        mz_uint16 m_version_made_by;
        mz_uint16 m_version_needed;
        mz_uint16 m_bit_flag;
        mz_uint16 m_method;

        /* CRC-32 of uncompressed data. */
        mz_uint32 m_crc32;

        /* File's compressed size. */
        mz_uint64 m_comp_size;

        /* File's uncompressed size. Note, I've seen some old archives where directory entries had 512 bytes for their uncompressed sizes, but when you try to unpack them you actually get 0 bytes. */
        mz_uint64 m_uncomp_size;

        /* Zip internal and external file attributes. */
        mz_uint16 m_internal_attr;
        mz_uint32 m_external_attr;

        /* Entry's local header file offset in bytes. */
        mz_uint64 m_local_header_ofs;

        /* Size of comment in bytes. */
        mz_uint32 m_comment_size;

        /* MZ_TRUE if the entry appears to be a directory. */
        mz_bool m_is_directory;

        /* MZ_TRUE if the entry uses encryption/strong encryption (which miniz_zip doesn't support) */
        mz_bool m_is_encrypted;

        /* MZ_TRUE if the file is not encrypted, a patch file, and if it uses a compression method we support. */
        mz_bool m_is_supported;

        /* Filename. If string ends in '/' it's a subdirectory entry. */
        /* Guaranteed to be zero terminated, may be truncated to fit. */
        char m_filename[MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE];

        /* Comment field. */
        /* Guaranteed to be zero terminated, may be truncated to fit. */
        char m_comment[MZ_ZIP_MAX_ARCHIVE_FILE_COMMENT_SIZE];

#ifdef MINIZ_NO_TIME
        MZ_TIME_T m_padding;
#else
        MZ_TIME_T m_time;
#endif
    } mz_zip_archive_file_stat;

    typedef size_t (*mz_file_read_func)(void *pOpaque, mz_uint64 file_ofs, void *pBuf, size_t n);
    typedef size_t (*mz_file_write_func)(void *pOpaque, mz_uint64 file_ofs, const void *pBuf, size_t n);
    typedef mz_bool (*mz_file_needs_keepalive)(void *pOpaque);

    struct mz_zip_internal_state_tag;
    typedef struct mz_zip_internal_state_tag mz_zip_internal_state;

    typedef enum
    {
        MZ_ZIP_MODE_INVALID = 0,
        MZ_ZIP_MODE_READING = 1,
        MZ_ZIP_MODE_WRITING = 2,
        MZ_ZIP_MODE_WRITING_HAS_BEEN_FINALIZED = 3
    } mz_zip_mode;

    typedef enum
    {
        MZ_ZIP_FLAG_CASE_SENSITIVE = 0x0100,
        MZ_ZIP_FLAG_IGNORE_PATH = 0x0200,
        MZ_ZIP_FLAG_COMPRESSED_DATA = 0x0400,
        MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY = 0x0800,
        MZ_ZIP_FLAG_VALIDATE_LOCATE_FILE_FLAG = 0x1000,
        MZ_ZIP_FLAG_VALIDATE_HEADERS_ONLY = 0x2000,
        MZ_ZIP_FLAG_WRITE_ZIP64 = 0x4000,
        MZ_ZIP_FLAG_WRITE_ALLOW_READING = 0x8000,
        MZ_ZIP_FLAG_ASCII_FILENAME = 0x10000,
        MZ_ZIP_FLAG_WRITE_HEADER_SET_SIZE = 0x20000,
        MZ_ZIP_FLAG_READ_ALLOW_WRITING = 0x40000
    } mz_zip_flags;

    typedef enum
    {
        MZ_ZIP_TYPE_INVALID = 0,
        MZ_ZIP_TYPE_USER,
        MZ_ZIP_TYPE_MEMORY,
        MZ_ZIP_TYPE_HEAP,
        MZ_ZIP_TYPE_FILE,
        MZ_ZIP_TYPE_CFILE,
        MZ_ZIP_TOTAL_TYPES
    } mz_zip_type;

    typedef enum
    {
        MZ_ZIP_NO_ERROR = 0,
        MZ_ZIP_UNDEFINED_ERROR,
        MZ_ZIP_TOO_MANY_FILES,
        MZ_ZIP_FILE_TOO_LARGE,
        MZ_ZIP_UNSUPPORTED_METHOD,
        MZ_ZIP_UNSUPPORTED_ENCRYPTION,
        MZ_ZIP_UNSUPPORTED_FEATURE,
        MZ_ZIP_FAILED_FINDING_CENTRAL_DIR,
        MZ_ZIP_NOT_AN_ARCHIVE,
        MZ_ZIP_INVALID_HEADER_OR_CORRUPTED,
        MZ_ZIP_UNSUPPORTED_MULTIDISK,
        MZ_ZIP_DECOMPRESSION_FAILED,
        MZ_ZIP_COMPRESSION_FAILED,
        MZ_ZIP_UNEXPECTED_DECOMPRESSED_SIZE,
        MZ_ZIP_CRC_CHECK_FAILED,
        MZ_ZIP_UNSUPPORTED_CDIR_SIZE,
        MZ_ZIP_ALLOC_FAILED,
        MZ_ZIP_FILE_OPEN_FAILED,
        MZ_ZIP_FILE_CREATE_FAILED,
        MZ_ZIP_FILE_WRITE_FAILED,
        MZ_ZIP_FILE_READ_FAILED,
        MZ_ZIP_FILE_CLOSE_FAILED,
        MZ_ZIP_FILE_SEEK_FAILED,
        MZ_ZIP_FILE_STAT_FAILED,
        MZ_ZIP_INVALID_PARAMETER,
        MZ_ZIP_INVALID_FILENAME,
        MZ_ZIP_BUF_TOO_SMALL,
        MZ_ZIP_INTERNAL_ERROR,
        MZ_ZIP_FILE_NOT_FOUND,
        MZ_ZIP_ARCHIVE_TOO_LARGE,
        MZ_ZIP_VALIDATION_FAILED,
        MZ_ZIP_WRITE_CALLBACK_FAILED,
        MZ_ZIP_TOTAL_ERRORS
    } mz_zip_error;

    typedef struct
    {
        mz_uint64 m_archive_size;
        mz_uint64 m_central_directory_file_ofs;

        /* We only support up to UINT32_MAX files in zip64 mode. */
        mz_uint32 m_total_files;
        mz_zip_mode m_zip_mode;
        mz_zip_type m_zip_type;
        mz_zip_error m_last_error;

        mz_uint64 m_file_offset_alignment;

        mz_alloc_func m_pAlloc;
        mz_free_func m_pFree;
        mz_realloc_func m_pRealloc;
        void *m_pAlloc_opaque;

        mz_file_read_func m_pRead;
        mz_file_write_func m_pWrite;
        mz_file_needs_keepalive m_pNeeds_keepalive;
        void *m_pIO_opaque;

        mz_zip_internal_state *m_pState;

    } mz_zip_archive;

    typedef struct
    {
        mz_zip_archive *pZip;
        mz_uint flags;

        int status;

        mz_uint64 read_buf_size, read_buf_ofs, read_buf_avail, comp_remaining, out_buf_ofs, cur_file_ofs;
        mz_zip_archive_file_stat file_stat;
        void *pRead_buf;
        void *pWrite_buf;

        size_t out_blk_remain;

        tinfl_decompressor inflator;

#ifdef MINIZ_DISABLE_ZIP_READER_CRC32_CHECKS
        mz_uint padding;
#else
        mz_uint file_crc32;
#endif

    } mz_zip_reader_extract_iter_state;

    /* -------- ZIP reading */

    MINIZ_EXPORT mz_bool mz_zip_reader_init(mz_zip_archive *pZip, mz_uint64 size, mz_uint flags);

    MINIZ_EXPORT mz_bool mz_zip_reader_init_mem(mz_zip_archive *pZip, const void *pMem, size_t size, mz_uint flags);

#ifndef MINIZ_NO_STDIO
    MINIZ_EXPORT mz_bool mz_zip_reader_init_file(mz_zip_archive *pZip, const char *pFilename, mz_uint32 flags);
    MINIZ_EXPORT mz_bool mz_zip_reader_init_file_v2(mz_zip_archive *pZip, const char *pFilename, mz_uint flags, mz_uint64 file_start_ofs, mz_uint64 archive_size);
    MINIZ_EXPORT mz_bool mz_zip_reader_init_cfile(mz_zip_archive *pZip, MZ_FILE *pFile, mz_uint64 archive_size, mz_uint flags);
#endif

    MINIZ_EXPORT mz_bool mz_zip_reader_end(mz_zip_archive *pZip);
    MINIZ_EXPORT void mz_zip_zero_struct(mz_zip_archive *pZip);
    MINIZ_EXPORT mz_zip_mode mz_zip_get_mode(mz_zip_archive *pZip);
    MINIZ_EXPORT mz_zip_type mz_zip_get_type(mz_zip_archive *pZip);
    MINIZ_EXPORT mz_uint mz_zip_reader_get_num_files(mz_zip_archive *pZip);
    MINIZ_EXPORT mz_uint64 mz_zip_get_archive_size(mz_zip_archive *pZip);
    MINIZ_EXPORT mz_uint64 mz_zip_get_archive_file_start_offset(mz_zip_archive *pZip);
    MINIZ_EXPORT MZ_FILE *mz_zip_get_cfile(mz_zip_archive *pZip);
    MINIZ_EXPORT size_t mz_zip_read_archive_data(mz_zip_archive *pZip, mz_uint64 file_ofs, void *pBuf, size_t n);
    MINIZ_EXPORT mz_zip_error mz_zip_set_last_error(mz_zip_archive *pZip, mz_zip_error err_num);
    MINIZ_EXPORT mz_zip_error mz_zip_peek_last_error(mz_zip_archive *pZip);
    MINIZ_EXPORT mz_zip_error mz_zip_clear_last_error(mz_zip_archive *pZip);
    MINIZ_EXPORT mz_zip_error mz_zip_get_last_error(mz_zip_archive *pZip);
    MINIZ_EXPORT const char *mz_zip_get_error_string(mz_zip_error mz_err);
    MINIZ_EXPORT mz_bool mz_zip_reader_is_file_a_directory(mz_zip_archive *pZip, mz_uint file_index);
    MINIZ_EXPORT mz_bool mz_zip_reader_is_file_encrypted(mz_zip_archive *pZip, mz_uint file_index);
    MINIZ_EXPORT mz_bool mz_zip_reader_is_file_supported(mz_zip_archive *pZip, mz_uint file_index);
    MINIZ_EXPORT mz_uint mz_zip_reader_get_filename(mz_zip_archive *pZip, mz_uint file_index, char *pFilename, mz_uint filename_buf_size);
    MINIZ_EXPORT int mz_zip_reader_locate_file(mz_zip_archive *pZip, const char *pName, const char *pComment, mz_uint flags);
    MINIZ_EXPORT mz_bool mz_zip_reader_locate_file_v2(mz_zip_archive *pZip, const char *pName, const char *pComment, mz_uint flags, mz_uint32 *file_index);
    MINIZ_EXPORT mz_bool mz_zip_reader_file_stat(mz_zip_archive *pZip, mz_uint file_index, mz_zip_archive_file_stat *pStat);
    MINIZ_EXPORT mz_bool mz_zip_is_zip64(mz_zip_archive *pZip);
    MINIZ_EXPORT size_t mz_zip_get_central_dir_size(mz_zip_archive *pZip);
    MINIZ_EXPORT mz_bool mz_zip_reader_extract_to_mem_no_alloc(mz_zip_archive *pZip, mz_uint file_index, void *pBuf, size_t buf_size, mz_uint flags, void *pUser_read_buf, size_t user_read_buf_size);
    MINIZ_EXPORT mz_bool mz_zip_reader_extract_file_to_mem_no_alloc(mz_zip_archive *pZip, const char *pFilename, void *pBuf, size_t buf_size, mz_uint flags, void *pUser_read_buf, size_t user_read_buf_size);
    MINIZ_EXPORT mz_bool mz_zip_reader_extract_to_mem(mz_zip_archive *pZip, mz_uint file_index, void *pBuf, size_t buf_size, mz_uint flags);
    MINIZ_EXPORT mz_bool mz_zip_reader_extract_file_to_mem(mz_zip_archive *pZip, const char *pFilename, void *pBuf, size_t buf_size, mz_uint flags);
    MINIZ_EXPORT void *mz_zip_reader_extract_to_heap(mz_zip_archive *pZip, mz_uint file_index, size_t *pSize, mz_uint flags);
    MINIZ_EXPORT void *mz_zip_reader_extract_file_to_heap(mz_zip_archive *pZip, const char *pFilename, size_t *pSize, mz_uint flags);
    MINIZ_EXPORT mz_bool mz_zip_reader_extract_to_callback(mz_zip_archive *pZip, mz_uint file_index, mz_file_write_func pCallback, void *pOpaque, mz_uint flags);
    MINIZ_EXPORT mz_bool mz_zip_reader_extract_file_to_callback(mz_zip_archive *pZip, const char *pFilename, mz_file_write_func pCallback, void *pOpaque, mz_uint flags);
    MINIZ_EXPORT mz_zip_reader_extract_iter_state *mz_zip_reader_extract_iter_new(mz_zip_archive *pZip, mz_uint file_index, mz_uint flags);
    MINIZ_EXPORT mz_zip_reader_extract_iter_state *mz_zip_reader_extract_file_iter_new(mz_zip_archive *pZip, const char *pFilename, mz_uint flags);
    MINIZ_EXPORT size_t mz_zip_reader_extract_iter_read(mz_zip_reader_extract_iter_state *pState, void *pvBuf, size_t buf_size);
    MINIZ_EXPORT mz_bool mz_zip_reader_extract_iter_free(mz_zip_reader_extract_iter_state *pState);
    MINIZ_EXPORT mz_bool mz_zip_validate_file(mz_zip_archive *pZip, mz_uint file_index, mz_uint flags);
    MINIZ_EXPORT mz_bool mz_zip_validate_archive(mz_zip_archive *pZip, mz_uint flags);
    MINIZ_EXPORT mz_bool mz_zip_validate_mem_archive(const void *pMem, size_t size, mz_uint flags, mz_zip_error *pErr);
#ifndef MINIZ_NO_STDIO
    MINIZ_EXPORT mz_bool mz_zip_validate_file_archive(const char *pFilename, mz_uint flags, mz_zip_error *pErr);
#endif
    MINIZ_EXPORT mz_bool mz_zip_end(mz_zip_archive *pZip);

#ifndef MINIZ_NO_ARCHIVE_WRITING_APIS

    MINIZ_EXPORT mz_bool mz_zip_writer_init(mz_zip_archive *pZip, mz_uint64 existing_size);
    MINIZ_EXPORT mz_bool mz_zip_writer_init_v2(mz_zip_archive *pZip, mz_uint64 existing_size, mz_uint flags);
    MINIZ_EXPORT mz_bool mz_zip_writer_init_heap(mz_zip_archive *pZip, size_t size_to_reserve_at_beginning, size_t initial_allocation_size);
    MINIZ_EXPORT mz_bool mz_zip_writer_init_heap_v2(mz_zip_archive *pZip, size_t size_to_reserve_at_beginning, size_t initial_allocation_size, mz_uint flags);

#ifndef MINIZ_NO_STDIO
    MINIZ_EXPORT mz_bool mz_zip_writer_init_file(mz_zip_archive *pZip, const char *pFilename, mz_uint64 size_to_reserve_at_beginning);
    MINIZ_EXPORT mz_bool mz_zip_writer_init_file_v2(mz_zip_archive *pZip, const char *pFilename, mz_uint64 size_to_reserve_at_beginning, mz_uint flags);
    MINIZ_EXPORT mz_bool mz_zip_writer_init_cfile(mz_zip_archive *pZip, MZ_FILE *pFile, mz_uint flags);
#endif

    MINIZ_EXPORT mz_bool mz_zip_writer_init_from_reader(mz_zip_archive *pZip, const char *pFilename);
    MINIZ_EXPORT mz_bool mz_zip_writer_init_from_reader_v2(mz_zip_archive *pZip, const char *pFilename, mz_uint flags);
    MINIZ_EXPORT mz_bool mz_zip_writer_add_mem(mz_zip_archive *pZip, const char *pArchive_name, const void *pBuf, size_t buf_size, mz_uint level_and_flags);
    MINIZ_EXPORT mz_bool mz_zip_writer_add_mem_ex(mz_zip_archive *pZip, const char *pArchive_name, const void *pBuf, size_t buf_size, const void *pComment, mz_uint16 comment_size, mz_uint level_and_flags,
                                                  mz_uint64 uncomp_size, mz_uint32 uncomp_crc32);

    MINIZ_EXPORT mz_bool mz_zip_writer_add_mem_ex_v2(mz_zip_archive *pZip, const char *pArchive_name, const void *pBuf, size_t buf_size, const void *pComment, mz_uint16 comment_size, mz_uint level_and_flags,
                                                     mz_uint64 uncomp_size, mz_uint32 uncomp_crc32, MZ_TIME_T *last_modified, const char *user_extra_data_local, mz_uint user_extra_data_local_len,
                                                     const char *user_extra_data_central, mz_uint user_extra_data_central_len);

#ifndef MINIZ_NO_STDIO
    MINIZ_EXPORT mz_bool mz_zip_writer_add_file(mz_zip_archive *pZip, const char *pArchive_name, const char *pSrc_filename, const void *pComment, mz_uint16 comment_size, mz_uint level_and_flags);
    MINIZ_EXPORT mz_bool mz_zip_writer_add_cfile(mz_zip_archive *pZip, const char *pArchive_name, MZ_FILE *pSrc_file, mz_uint64 max_size,
                                                 const MZ_TIME_T *pFile_time, const void *pComment, mz_uint16 comment_size, mz_uint level_and_flags, const char *user_extra_data_local, mz_uint user_extra_data_local_len,
                                                 const char *user_extra_data_central, mz_uint user_extra_data_central_len);
#endif

    MINIZ_EXPORT mz_bool mz_zip_writer_add_from_zip_reader(mz_zip_archive *pZip, mz_zip_archive *pSource_zip, mz_uint src_file_index);
    MINIZ_EXPORT mz_bool mz_zip_writer_finalize_archive(mz_zip_archive *pZip);
    MINIZ_EXPORT mz_bool mz_zip_writer_finalize_heap_archive(mz_zip_archive *pZip, void **ppBuf, size_t *pSize);
    MINIZ_EXPORT mz_bool mz_zip_writer_end(mz_zip_archive *pZip);
    MINIZ_EXPORT mz_bool mz_zip_add_mem_to_archive_file_in_place(const char *pZip_filename, const char *pArchive_name, const void *pBuf, size_t buf_size, const void *pComment, mz_uint16 comment_size, mz_uint level_and_flags);
    MINIZ_EXPORT mz_bool mz_zip_add_mem_to_archive_file_in_place_v2(const char *pZip_filename, const char *pArchive_name, const void *pBuf, size_t buf_size, const void *pComment, mz_uint16 comment_size, mz_uint level_and_flags, mz_zip_error *pErr);

#ifndef MINIZ_NO_STDIO
    MINIZ_EXPORT void *mz_zip_extract_archive_file_to_heap(const char *pZip_filename, const char *pArchive_name, size_t *pSize, mz_uint flags);
    MINIZ_EXPORT void *mz_zip_extract_archive_file_to_heap_v2(const char *pZip_filename, const char *pArchive_name, const char *pComment, size_t *pSize, mz_uint flags, mz_zip_error *pErr);
#endif

#endif /* #ifndef MINIZ_NO_ARCHIVE_WRITING_APIS */

#ifdef __cplusplus
}
#endif

#endif /* MINIZ_NO_ARCHIVE_APIS */
