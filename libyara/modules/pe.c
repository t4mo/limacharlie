/*
Copyright (c) 2014. The YARA Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifdef _WIN32
#include <windows.h>
#else
#include <yara/pe.h>
#endif

#include <yara/modules.h>
#include <yara/mem.h>
#include <yara/strutils.h>

#define MODULE_NAME pe

#define RESOURCE_TYPE_CURSOR         1
#define RESOURCE_TYPE_BITMAP         2
#define RESOURCE_TYPE_ICON           3
#define RESOURCE_TYPE_MENU           4
#define RESOURCE_TYPE_DIALOG         5
#define RESOURCE_TYPE_STRING         6
#define RESOURCE_TYPE_FONTDIR        7
#define RESOURCE_TYPE_FONT           8
#define RESOURCE_TYPE_ACCELERATOR    9
#define RESOURCE_TYPE_RCDATA         10
#define RESOURCE_TYPE_MESSAGETABLE   11
#define RESOURCE_TYPE_VERSION        16
#define RESOURCE_TYPE_MANIFEST       24


#define RESOURCE_CALLBACK_CONTINUE   0
#define RESOURCE_CALLBACK_ABORT      1


#define RESOURCE_ITERATOR_FINISHED   0
#define RESOURCE_ITERATOR_ABORTED    1


#define MAX_PE_SECTIONS              96


#define IS_RESOURCE_SUBDIRECTORY(entry) \
    ((entry)->OffsetToData & 0x80000000)


#define RESOURCE_OFFSET(entry) \
    ((entry)->OffsetToData & 0x7FFFFFFF)


typedef int (*RESOURCE_CALLBACK_FUNC) \
    (int type, int id, int language, void* cb_data);


typedef struct _DATA
{
  PIMAGE_NT_HEADERS32 pe_header;

  size_t pe_size;
  size_t pe_offset;

  uint8_t* data;
  size_t data_size;

} DATA;


PIMAGE_NT_HEADERS32 pe_get_header(
    uint8_t* buffer,
    size_t buffer_length)
{
  PIMAGE_DOS_HEADER mz_header;
  PIMAGE_NT_HEADERS32 pe_header;

  size_t headers_size = 0;

  if (buffer_length < sizeof(IMAGE_DOS_HEADER))
    return NULL;

  mz_header = (PIMAGE_DOS_HEADER) buffer;

  if (mz_header->e_magic != IMAGE_DOS_SIGNATURE)
    return NULL;

  if (mz_header->e_lfanew < 0)
    return NULL;

  headers_size = mz_header->e_lfanew + \
                 sizeof(pe_header->Signature) + \
                 sizeof(IMAGE_FILE_HEADER);

  if (buffer_length < headers_size)
    return NULL;

  pe_header = (PIMAGE_NT_HEADERS32) (buffer + mz_header->e_lfanew);

  headers_size += pe_header->FileHeader.SizeOfOptionalHeader;

  if (pe_header->Signature == IMAGE_NT_SIGNATURE &&
      (pe_header->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 ||
       pe_header->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64) &&
      buffer_length > headers_size)
  {
    return pe_header;
  }
  else
  {
    return NULL;
  }
}


PIMAGE_DATA_DIRECTORY pe_get_directory_entry(
    PIMAGE_NT_HEADERS32 pe_header,
    int entry)
{
  PIMAGE_DATA_DIRECTORY result;

  if (pe_header->FileHeader.Machine == 0x8664)  // is a 64-bit PE ?
    result = &((PIMAGE_NT_HEADERS64) pe_header)->
        OptionalHeader.DataDirectory[entry];
  else
    result = &pe_header->OptionalHeader.DataDirectory[entry];

  return result;
}


uint64_t pe_rva_to_offset(
    PIMAGE_NT_HEADERS32 pe_header,
    size_t pe_size,
    uint64_t rva)
{
  PIMAGE_SECTION_HEADER section;
  DWORD section_rva;
  DWORD section_offset;

  int i = 0;

  section = IMAGE_FIRST_SECTION(pe_header);
  section_rva = 0;
  section_offset = 0;

  while(i < min(pe_header->FileHeader.NumberOfSections, MAX_PE_SECTIONS))
  {
    if ((uint8_t*) section - \
        (uint8_t*) pe_header + sizeof(IMAGE_SECTION_HEADER) < pe_size)
    {
      if (rva >= section->VirtualAddress &&
          section_rva <= section->VirtualAddress)
      {
        section_rva = section->VirtualAddress;
        section_offset = section->PointerToRawData;
      }

      section++;
      i++;
    }
    else
    {
      return 0;
    }
  }

  return section_offset + (rva - section_rva);
}


int _pe_iterate_resources(
    PIMAGE_RESOURCE_DIRECTORY resource_dir,
    uint8_t* rsrc_data,
    size_t rsrc_size,
    int rsrc_tree_level,
    int* type,
    int* id,
    int* language,
    RESOURCE_CALLBACK_FUNC callback,
    void* callback_data)
{
  int result;
  int total_entries = resource_dir->NumberOfNamedEntries +
                      resource_dir->NumberOfIdEntries;

  PIMAGE_RESOURCE_DIRECTORY_ENTRY entry = &resource_dir->DirectoryEntries[0];

  for (int i = 0; i < total_entries; i++)
  {
    switch(rsrc_tree_level)
    {
      case 0:
        *type = entry->Name;
        break;
      case 1:
        *id = entry->Name;
        break;
      case 2:
        *language = entry->Name;
        break;
    }

    if (IS_RESOURCE_SUBDIRECTORY(entry))
    {
      result = _pe_iterate_resources(
          (PIMAGE_RESOURCE_DIRECTORY) (rsrc_data + RESOURCE_OFFSET(entry)),
          rsrc_data,
          rsrc_size,
          rsrc_tree_level + 1,
          type,
          id,
          language,
          callback,
          callback_data);

      if (result == RESOURCE_ITERATOR_ABORTED)
        return RESOURCE_ITERATOR_ABORTED;
    }
    else
    {
      result = callback(*type, *id, *language, callback_data);

      if (result == RESOURCE_CALLBACK_ABORT)
        return RESOURCE_ITERATOR_ABORTED;
    }

    if (result == RESOURCE_ITERATOR_ABORTED)
      return result;

    entry++;
  }

  return RESOURCE_ITERATOR_FINISHED;
}


int pe_iterate_resources(
    PIMAGE_NT_HEADERS32 pe_header,
    size_t pe_size,
    size_t pe_offset,
    RESOURCE_CALLBACK_FUNC callback,
    void* callback_data)
{
  uint64_t offset;

  int type = -1;
  int id = -1;
  int language = -1;

  PIMAGE_DATA_DIRECTORY directory = pe_get_directory_entry(
      pe_header, IMAGE_DIRECTORY_ENTRY_RESOURCE);

  if (directory->VirtualAddress != 0)
  {
    offset = pe_rva_to_offset(
        pe_header, pe_size, directory->VirtualAddress);

    if (offset != 0 &&
        offset < pe_size &&
        directory->Size < pe_size - offset)
    {
      _pe_iterate_resources(
          (PIMAGE_RESOURCE_DIRECTORY) \
              ((uint8_t*) pe_header - pe_offset + offset),
          (uint8_t*) pe_header - pe_offset + offset,
          directory->Size,
          0,
          &type,
          &id,
          &language,
          callback,
          callback_data);

      return 1;
    }
  }

  return 0;
}


void pe_parse_header(
    PIMAGE_NT_HEADERS32 pe,
    size_t base_address,
    size_t pe_size,
    int flags,
    YR_OBJECT* pe_obj)
{
  PIMAGE_SECTION_HEADER section;

  char section_name[IMAGE_SIZEOF_SHORT_NAME + 1];
  int i;

#define OptionalHeader(field) \
  (pe->FileHeader.Machine == 0x8664 ? \
   ((PIMAGE_NT_HEADERS64) pe)->OptionalHeader.field: pe->OptionalHeader.field)

  set_integer(
      pe->FileHeader.Machine,
      pe_obj, "machine");

  set_integer(
      pe->FileHeader.NumberOfSections,
      pe_obj, "number_of_sections");

  set_integer(
      pe->FileHeader.TimeDateStamp,
      pe_obj, "timestamp");

  set_integer(
      pe->FileHeader.Characteristics,
      pe_obj, "characteristics");

  set_integer(
      flags & SCAN_FLAGS_PROCESS_MEMORY ?
        base_address + OptionalHeader(AddressOfEntryPoint) :
        pe_rva_to_offset(
            pe, pe_size, OptionalHeader(AddressOfEntryPoint)),
      pe_obj, "entry_point");

  set_integer(
      OptionalHeader(ImageBase),
      pe_obj, "image_base");

  set_integer(
      OptionalHeader(MajorLinkerVersion),
      pe_obj, "linker_version.major");

  set_integer(
      OptionalHeader(MinorLinkerVersion),
      pe_obj, "linker_version.minor");

  set_integer(
      OptionalHeader(MajorOperatingSystemVersion),
      pe_obj, "os_version.major");

  set_integer(
      OptionalHeader(MinorOperatingSystemVersion),
      pe_obj, "os_version.minor");

  set_integer(
      OptionalHeader(MajorImageVersion),
      pe_obj, "image_version.major");

  set_integer(
      OptionalHeader(MinorImageVersion),
      pe_obj, "image_version.minor");

  set_integer(
      OptionalHeader(MajorSubsystemVersion),
      pe_obj, "subsystem_version.major");

  set_integer(
      OptionalHeader(MinorSubsystemVersion),
      pe_obj, "subsystem_version.minor");

  set_integer(
      OptionalHeader(Subsystem),
      pe_obj, "subsystem");

  section = IMAGE_FIRST_SECTION(pe);

  for (i = 0; i < min(pe->FileHeader.NumberOfSections, MAX_PE_SECTIONS); i++)
  {
    if ((uint8_t*) section -
        (uint8_t*) pe + sizeof(IMAGE_SECTION_HEADER) >= pe_size)
    {
      break;
    }

    strlcpy(section_name, (char*) section->Name, IMAGE_SIZEOF_SHORT_NAME + 1);

    set_string(
        section_name,
        pe_obj, "sections[%i].name", i);

    set_integer(
        section->Characteristics,
        pe_obj, "sections[%i].characteristics", i);

    set_integer(section->SizeOfRawData,
        pe_obj, "sections[%i].raw_data_size", i);

    set_integer(section->PointerToRawData,
        pe_obj, "sections[%i].raw_data_offset", i);

    set_integer(section->VirtualAddress,
        pe_obj, "sections[%i].virtual_address", i);

    set_integer(
        section->Misc.VirtualSize,
        pe_obj, "sections[%i].virtual_size", i);

    section++;
  }
}


define_function(section_index)
{
  YR_OBJECT* module = module();
  char* name = string_argument(1);

  int64_t n = get_integer(module, "number_of_sections");
  int64_t i;

  if (n == UNDEFINED)
    return_integer(UNDEFINED);

  for (i = 0; i < n; i++)
  {
    if (strcmp(name, get_string(module, "sections[%i].name", i)) == 0)
      return_integer(i);
  }

  return_integer(UNDEFINED);
}


define_function(exports)
{
  char* function_name = string_argument(1);

  YR_OBJECT* module = module();
  DATA* data = (DATA*) module->data;

  PIMAGE_DATA_DIRECTORY directory;
  PIMAGE_EXPORT_DIRECTORY exports;
  DWORD* names;

  char* name;
  int i;
  uint64_t offset;

  // if not a PE file, return UNDEFINED

  if (data == NULL)
    return_integer(UNDEFINED);

  directory = pe_get_directory_entry(
      data->pe_header,
      IMAGE_DIRECTORY_ENTRY_EXPORT);

  // if the PE doesn't export any functions, return FALSE

  if (directory->VirtualAddress == 0)
    return_integer(0);

  offset = pe_rva_to_offset(
      data->pe_header,
      data->pe_size,
      directory->VirtualAddress);

  if (offset == 0 ||
      offset >= data->data_size)
    return_integer(0);

  exports = (PIMAGE_EXPORT_DIRECTORY)(data->data + offset);

  offset = pe_rva_to_offset(
      data->pe_header,
      data->pe_size,
      exports->AddressOfNames);

  if (offset == 0 ||
      offset + exports->NumberOfNames * sizeof(DWORD) > data->data_size)
    return_integer(0);

  names = (DWORD*)(data->data + offset);

  for (i = 0; i < exports->NumberOfNames; i++)
  {
    offset = pe_rva_to_offset(
        data->pe_header,
        data->pe_size,
        names[i]);

    if (offset == 0 || offset >= data->data_size)
      return_integer(0);

    name = (char*)(data->data + offset);

    if (strncmp(name, function_name, data->data_size - offset) == 0)
      return_integer(1);
  }

  return_integer(0);
}


#define check_bounds(pointer, struct_type, limit) \
    ((uint8_t*)(pointer) + sizeof(struct_type) <= limit)

define_function(imports)
{
  char* dll_name = string_argument(1);
  char* function_name = string_argument(2);
  int function_name_len = strlen(function_name);

  YR_OBJECT* module = module();
  DATA* data = (DATA*) module->data;

  PIMAGE_DATA_DIRECTORY directory;
  PIMAGE_IMPORT_DESCRIPTOR imports;
  PIMAGE_IMPORT_BY_NAME import;
  PIMAGE_THUNK_DATA32 thunks32;
  PIMAGE_THUNK_DATA64 thunks64;

  uint8_t* data_end;
  uint64_t offset;

  // if not a PE file, return UNDEFINED

  if (data == NULL)
    return_integer(UNDEFINED);

  data_end = data->data + data->data_size;

  directory = pe_get_directory_entry(
      data->pe_header,
      IMAGE_DIRECTORY_ENTRY_IMPORT);

  if (directory->VirtualAddress == 0)
    return_integer(0);

  offset = pe_rva_to_offset(
      data->pe_header,
      data->pe_size,
      directory->VirtualAddress);

  if (offset == 0 ||
      offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) > data->data_size)
    return_integer(0);

  imports = (PIMAGE_IMPORT_DESCRIPTOR)(data->data + offset);

  while (check_bounds(imports, IMAGE_IMPORT_DESCRIPTOR, data_end) &&
         imports->Name != 0)
  {
    offset = pe_rva_to_offset(
        data->pe_header,
        data->pe_size,
        imports->Name);

    if (offset > 0 &&
        offset <= data->data_size &&
        strncasecmp(
            dll_name,
            (char*)(data->data + offset),
            data->data_size - offset) == 0)
    {
      offset = pe_rva_to_offset(
          data->pe_header,
          data->pe_size,
          imports->OriginalFirstThunk);

      if (offset > 0)
      {
        if (data->pe_header->FileHeader.Machine == 0x8664)
        {
          thunks64 = (PIMAGE_THUNK_DATA64)(data->data + offset);

          while (check_bounds(thunks64, IMAGE_THUNK_DATA64, data_end) &&
                 thunks64->u1.Ordinal != 0)
          {
            if (!(thunks64->u1.Ordinal & IMAGE_ORDINAL_FLAG64))
            {
              // if not exported by ordinal
              offset = pe_rva_to_offset(
                  data->pe_header,
                  data->pe_size,
                  thunks64->u1.Function);

              if (offset != 0 &&
                  offset <= data->data_size - sizeof(IMAGE_IMPORT_BY_NAME))
              {
                import = (PIMAGE_IMPORT_BY_NAME)(data->data + offset);

                if (data_end - import->Name >= function_name_len)
                {
                  if (strncmp((char*) import->Name,
                              function_name,
                              function_name_len) == 0)
                  {
                    return_integer(1);
                  }
                }
              }
            }

            thunks64++;
          }
        }
        else
        {
          thunks32 = (PIMAGE_THUNK_DATA32)(data->data + offset);

          while (check_bounds(thunks32, IMAGE_THUNK_DATA32, data_end) &&
                 thunks32->u1.Ordinal != 0)
          {
            if (!(thunks32->u1.Ordinal & IMAGE_ORDINAL_FLAG32))
            {
              // if not exported by ordinal
              offset = pe_rva_to_offset(
                  data->pe_header,
                  data->pe_size,
                  thunks32->u1.Function);

              if (offset != 0 &&
                  offset <= data->data_size - sizeof(IMAGE_IMPORT_BY_NAME))
              {
                import = (PIMAGE_IMPORT_BY_NAME)(data->data + offset);

                if (data_end - import->Name >= function_name_len)
                {
                  if (strncmp((char*) import->Name,
                              function_name,
                              function_name_len) == 0)
                  {
                    return_integer(1);
                  }
                }
              }
            }

            thunks32++;
          }
        }
      }
    }

    imports++;
  }

  return_integer(0);
}


typedef struct _FIND_LANGUAGE_CB_DATA
{
  uint64_t language;
  int found;

} FIND_LANGUAGE_CB_DATA;


int pe_find_language_cb(
    int rsrc_type,
    int rsrc_id,
    int rsrc_language,
    FIND_LANGUAGE_CB_DATA* cb_data)
{
  if (rsrc_language == cb_data->language)
  {
    cb_data->found = TRUE;
    return RESOURCE_CALLBACK_ABORT;
  }

  return RESOURCE_CALLBACK_CONTINUE;
}


define_function(language)
{
  FIND_LANGUAGE_CB_DATA cb_data;

  cb_data.language = integer_argument(1);
  cb_data.found = FALSE;

  YR_OBJECT* module = module();
  DATA* data = (DATA*) module->data;

  // if not a PE file, return UNDEFINED

  if (data == NULL)
    return_integer(UNDEFINED);

  if (pe_iterate_resources(
      data->pe_header,
      data->pe_size,
      data->pe_offset,
      (RESOURCE_CALLBACK_FUNC) pe_find_language_cb,
      (void*) &cb_data))
  {
    return_integer(cb_data.found);
  }
  else
  {
    return_integer(UNDEFINED);
  }
}

begin_declarations;

  declare_integer("MACHINE_I386");
  declare_integer("MACHINE_AMD64");

  declare_integer("SUBSYSTEM_UNKNOWN");
  declare_integer("SUBSYSTEM_NATIVE");
  declare_integer("SUBSYSTEM_WINDOWS_GUI");
  declare_integer("SUBSYSTEM_WINDOWS_CUI");
  declare_integer("SUBSYSTEM_OS2_CUI");
  declare_integer("SUBSYSTEM_POSIX_CUI");
  declare_integer("SUBSYSTEM_NATIVE_WINDOWS");

  declare_integer("RELOCS_STRIPPED");
  declare_integer("EXECUTABLE_IMAGE");
  declare_integer("LINE_NUMS_STRIPPED");
  declare_integer("LOCAL_SYMS_STRIPPED");
  declare_integer("AGGRESIVE_WS_TRIM");
  declare_integer("LARGE_ADDRESS_AWARE");
  declare_integer("BYTES_REVERSED_LO");
  declare_integer("32BIT_MACHINE");
  declare_integer("DEBUG_STRIPPED");
  declare_integer("REMOVABLE_RUN_FROM_SWAP");
  declare_integer("NET_RUN_FROM_SWAP");
  declare_integer("SYSTEM");
  declare_integer("DLL");
  declare_integer("UP_SYSTEM_ONLY");
  declare_integer("BYTES_REVERSED_HI");

  declare_integer("machine");
  declare_integer("number_of_sections");
  declare_integer("timestamp");
  declare_integer("characteristics");

  declare_integer("entry_point");
  declare_integer("image_base");

  begin_struct("linker_version");
    declare_integer("major");
    declare_integer("minor");
  end_struct("linker_version");

  begin_struct("os_version");
    declare_integer("major");
    declare_integer("minor");
  end_struct("os_version");

  begin_struct("image_version");
    declare_integer("major");
    declare_integer("minor");
  end_struct("image_version");

  begin_struct("subsystem_version");
    declare_integer("major");
    declare_integer("minor");
  end_struct("subsystem_version");

  declare_integer("subsystem");

  begin_struct_array("sections");
    declare_string("name");
    declare_integer("characteristics");
    declare_integer("virtual_address");
    declare_integer("virtual_size");
    declare_integer("raw_data_offset");
    declare_integer("raw_data_size");
  end_struct_array("sections");

  declare_function("section_index", "s", "i", section_index);
  declare_function("exports", "s", "i", exports);
  declare_function("imports", "ss", "i", imports);
  declare_function("language", "i", "i", language);

end_declarations;


int module_initialize(
    YR_MODULE* module)
{
  return ERROR_SUCCESS;
}


int module_finalize(
    YR_MODULE* module)
{
  return ERROR_SUCCESS;
}


int module_load(
    YR_SCAN_CONTEXT* context,
    YR_OBJECT* module_object,
    void* module_data,
    size_t module_data_size)
{
  PIMAGE_NT_HEADERS32 pe_header;
  YR_MEMORY_BLOCK* block;
  DATA* data;

  size_t pe_size;

  set_integer(
      IMAGE_FILE_MACHINE_I386, module_object,
      "MACHINE_I386");
  set_integer(
      IMAGE_FILE_MACHINE_AMD64, module_object,
      "MACHINE_AMD64");

  set_integer(
      IMAGE_SUBSYSTEM_UNKNOWN, module_object,
      "SUBSYSTEM_UNKNOWN");
  set_integer(
      IMAGE_SUBSYSTEM_NATIVE, module_object,
      "SUBSYSTEM_NATIVE");
  set_integer(
      IMAGE_SUBSYSTEM_WINDOWS_GUI, module_object,
      "SUBSYSTEM_WINDOWS_GUI");
  set_integer(
      IMAGE_SUBSYSTEM_WINDOWS_CUI, module_object,
      "SUBSYSTEM_WINDOWS_CUI");
  set_integer(
      IMAGE_SUBSYSTEM_OS2_CUI, module_object,
      "SUBSYSTEM_OS2_CUI");
  set_integer(
      IMAGE_SUBSYSTEM_POSIX_CUI, module_object,
      "SUBSYSTEM_POSIX_CUI");
  set_integer(
      IMAGE_SUBSYSTEM_NATIVE_WINDOWS, module_object,
      "SUBSYSTEM_NATIVE_WINDOWS");

  set_integer(
      IMAGE_FILE_RELOCS_STRIPPED, module_object,
      "RELOCS_STRIPPED");
  set_integer(
      IMAGE_FILE_EXECUTABLE_IMAGE, module_object,
      "EXECUTABLE_IMAGE");
  set_integer(
      IMAGE_FILE_LINE_NUMS_STRIPPED, module_object,
      "LINE_NUMS_STRIPPED");
  set_integer(
      IMAGE_FILE_LOCAL_SYMS_STRIPPED, module_object,
      "LOCAL_SYMS_STRIPPED");
  set_integer(
      IMAGE_FILE_AGGRESIVE_WS_TRIM, module_object,
      "AGGRESIVE_WS_TRIM");
  set_integer(
      IMAGE_FILE_LARGE_ADDRESS_AWARE, module_object,
      "LARGE_ADDRESS_AWARE");
  set_integer(
      IMAGE_FILE_BYTES_REVERSED_LO, module_object,
      "BYTES_REVERSED_LO");
  set_integer(
      IMAGE_FILE_32BIT_MACHINE, module_object,
      "32BIT_MACHINE");
  set_integer(
      IMAGE_FILE_DEBUG_STRIPPED, module_object,
      "DEBUG_STRIPPED");
  set_integer(
      IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP, module_object,
      "REMOVABLE_RUN_FROM_SWAP");
  set_integer(
      IMAGE_FILE_NET_RUN_FROM_SWAP, module_object,
      "NET_RUN_FROM_SWAP");
  set_integer(
      IMAGE_FILE_SYSTEM, module_object,
      "SYSTEM");
  set_integer(
      IMAGE_FILE_DLL, module_object,
      "DLL");
  set_integer(
      IMAGE_FILE_UP_SYSTEM_ONLY, module_object,
      "UP_SYSTEM_ONLY");
  set_integer(
      IMAGE_FILE_BYTES_REVERSED_HI, module_object,
      "BYTES_REVERSED_HI");

  foreach_memory_block(context, block)
  {
    pe_header = pe_get_header(block->data, block->size);

    if (pe_header != NULL)
    {
      // ignore DLLs while scanning a process

      if (!(context->flags & SCAN_FLAGS_PROCESS_MEMORY) ||
          !(pe_header->FileHeader.Characteristics & IMAGE_FILE_DLL))
      {
        pe_size = block->size - ((uint8_t*) pe_header - block->data);

        pe_parse_header(
            pe_header,
            block->base,
            pe_size,
            context->flags,
            module_object);

        data = (DATA*) yr_malloc(sizeof(DATA));

        if (data == NULL)
          return ERROR_INSUFICIENT_MEMORY;

        data->data = block->data;
        data->data_size = block->size;
        data->pe_header = pe_header;
        data->pe_size = pe_size;
        data->pe_offset = (uint8_t*) pe_header - block->data;

        module_object->data = data;
        break;
      }
    }
  }

  return ERROR_SUCCESS;
}


int module_unload(YR_OBJECT* module_object)
{
  if (module_object->data != NULL)
    yr_free(module_object->data);

  return ERROR_SUCCESS;
}
