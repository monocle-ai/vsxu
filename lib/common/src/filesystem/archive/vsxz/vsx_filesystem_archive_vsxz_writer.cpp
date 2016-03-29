#include <filesystem/archive/vsxz/vsx_filesystem_archive_vsxz_writer.h>
#include <filesystem/vsx_filesystem_helper.h>
#include <vsx_compression_lzma.h>
#include <vsx_compression_lzham.h>
#include <tools/vsx_thread_pool.h>
#include <filesystem/archive/vsxz/vsx_filesystem_archive_vsxz_header.h>
#include <filesystem/archive/vsxz/vsx_filesystem_archive_chunk_write.h>
#include <filesystem/vsx_filesystem_identifier.h>
#include <filesystem/tree/vsx_filesystem_tree_serialize_binary.h>

namespace vsx
{
void filesystem_archive_vsxz_writer::create(const char* filename)
{
  archive_filename = filename;
}

void filesystem_archive_vsxz_writer::add_file
(
  vsx_string<> filename,
  vsx_string<> disk_filename,
  bool deferred_multithreaded
)
{
  VSX_UNUSED(deferred_multithreaded);
  filesystem_archive_file_write file_info;
  file_info.operation = filesystem_archive_file_write::operation_add;
  file_info.filename = filename;
  file_info.source_filename = disk_filename;
  if (!disk_filename.size())
    file_info.source_filename = filename;

  archive_files.move_back(std::move(file_info));
}


void filesystem_archive_vsxz_writer::add_string(vsx_string<> filename, vsx_string<> payload, bool deferred_multithreaded)
{
  filesystem_archive_file_write file_info;
  file_info.filename = filename;
  req(payload.size());
  file_info.data.allocate( payload.size() - 1 );
  file_info.operation = filesystem_archive_file_write::operation_add;

  foreach (payload, i)
    file_info.data[i] = (unsigned char)payload[i];

  archive_files.move_back(std::move(file_info));
}


void filesystem_archive_vsxz_writer::archive_files_saturate_all()
{
  foreach (archive_files, i)
  {
    filesystem_archive_file_write &archive_file = archive_files[i];
    if (archive_file.data.size())
      continue;
    req_continue(archive_file.operation == filesystem_archive_file_write::operation_add);
    archive_file.data = filesystem_helper::file_read(archive_file.source_filename);
  }
}

void filesystem_archive_vsxz_writer::close()
{
  // Read all files from disk
  archive_files_saturate_all();

  // add files to compression chunks
  filesystem_archive_chunk_write chunks[8];
  size_t chunk_other_iterator = 0;
  foreach (archive_files, i)
  {
    // peek data, handle text files
    if (filesystem_identifier::is_text_file(archive_files[i].data))
    {
      chunks[1].add_file( &archive_files[i] );
      continue;
    }

    // if file is large, try to compress it, if ratio is too low, schedule in uncompressed chunk
    if (archive_files[i].data.size() > 1024*1024)
    {
      float lzma_ratio;
      threaded_task {
        vsx_ma_vector<unsigned char> lzma_compressed = vsx::compression_lzma::compress( archive_files[i].data );
        lzma_ratio = (float)(lzma_compressed.size()) / (float)(archive_files[i].data.size());
      } threaded_task_end;

      float lzham_ratio;
      threaded_task {
        vsx_ma_vector<unsigned char> lzham_compressed = vsx::compression_lzham::compress( archive_files[i].data );
        lzham_ratio = (float)(lzham_compressed.size()) / (float)(archive_files[i].data.size());
      } threaded_task_end;

      threaded_task_wait_all;

      if ( lzma_ratio > 0.95 && lzham_ratio > 0.95)
      {
        chunks[0].add_file( &archive_files[i] );
        continue;
      }
    }

    vsx_printf(L"adding file %s to chunk %d\n", archive_files[i].filename.c_str(), 2 + chunk_other_iterator);
    chunks[2 + chunk_other_iterator].add_file( &archive_files[i] );

    // when chunks reach 1 MB, start rotating
    if (chunks[2 + chunk_other_iterator].is_size_above_treshold())
    {
      chunk_other_iterator++;
      if (chunk_other_iterator == 6)
        chunk_other_iterator = 0;
    }
  }

  // set chunk id in all file info structs
  for (size_t i = 0; i < 8; i++)
    chunks[i].set_chunk_id(i);

  // add to file tree
  vsx_filesystem_tree_writer tree;
  size_t file_list_index = 1;
  for (size_t i = 0; i < 8; i++)
    chunks[i].add_to_tree( tree, file_list_index);

  // compress chunks
  for (size_t i = 1; i < 8; i++)
    chunks[i].compress();
  threaded_task_wait_all;

  // count valid chunks
  size_t valid_chunk_index = 2;
  for (; valid_chunk_index < 8; valid_chunk_index++)
    if (!chunks[valid_chunk_index].has_files())
      break;

  // serialize tree
  vsx_ma_vector<unsigned char> tree_data = vsx_filesystem_tree_serialize_binary::serialize(tree);

  // calculate & fill in header
  vsxz_header header;
  header.file_count = archive_files.size();
  header.chunk_count = valid_chunk_index;

  for (size_t i = 0; i < 8; i++)
    header.compression_uncompressed_memory_size += chunks[i].get_compressed_uncompressed_size();

  header.file_count = archive_files.size();
  header.tree_size = tree_data.size();

  // write archive to disk
  FILE* file = fopen(archive_filename.c_str(),"wb");
  fwrite(&header, sizeof(vsxz_header), 1, file);
  fwrite(tree_data.get_pointer(), 1, tree_data.get_sizeof(), file);

  for (size_t i = 0; i < 8; i++)
    chunks[i].write_file_info_table(file);

  chunks[0].write_chunk_info_table(file, true);
  chunks[1].write_chunk_info_table(file, true);
  for (size_t i = 2; i < 8; i++)
    chunks[i].write_chunk_info_table(file);

  for (size_t i = 0; i < 8; i++)
    chunks[i].write_data(file);

  fclose(file);
}

}
