# Utilities.cmake
# Supporting functions to build Jemalloc

########################################################################
# CheckTypeSize
function(UtilCheckTypeSize type OUTPUT_VAR_NAME)

CHECK_TYPE_SIZE(${type} ${OUTPUT_VAR_NAME} LANGUAGE C)

if(${${OUTPUT_VAR_NAME}})
  message (STATUS "${type} size is ${${OUTPUT_VAR_NAME}}")
  set(${OUTPUT_VAR_NAME} ${${OUTPUT_VAR_NAME}} PARENT_SCOPE)
else()
  message(FATAL_ERROR "Can not determine ${type} size")
endif()

endfunction(UtilCheckTypeSize)

########################################################################
# Power of two
# returns result in a VAR whose name is in RESULT_NAME
function (pow2 e RESULT_NAME)
  set(pow2_result 1)
  while ( ${e} GREATER 0 )
    math(EXPR pow2_result "${pow2_result} + ${pow2_result}")
    math(EXPR e "${e} - 1")
  endwhile(${e} GREATER 0 )
  set(${RESULT_NAME} ${pow2_result} PARENT_SCOPE)
endfunction(pow2)

#########################################################################
# Logarithm base 2
# returns result in a VAR whose name is in RESULT_NAME
function (lg x RESULT_NAME)
  set(lg_result 0)
  while ( ${x} GREATER 1 )
    math(EXPR lg_result "${lg_result} + 1")
    math(EXPR x "${x} / 2")
  endwhile ( ${x} GREATER 1 )
  set(${RESULT_NAME} ${lg_result} PARENT_SCOPE)
endfunction(lg)

######################################################
# Based on size_class() in size_classes.sh
#
# Thankfully, no floating point calcs
#
# Defined upon return:
# - lg_delta_lookup (${lg_delta} or "no")
# - bin ("yes" or "no")
function(size_class index lg_grp lg_delta ndelta lg_p lg_g lg_kmax output_file)

  lg( ${ndelta} "lg_ndelta") 
  pow2(${lg_ndelta} "pow2_result")
  
  if( ${pow2_result} LESS ${ndelta})
    set(rem "yes")
  else()
    set(rem "no")
  endif()

  set(lg_size ${lg_grp})
  
  math(EXPR lg_delta_plus_ndelta "${lg_delta} + ${lg_ndelta}")
  if( ${lg_delta_plus_ndelta} EQUAL ${lg_grp})
    math(EXPR lg_size "${lg_grp} + 1")
  else()
    set(lg_size ${lg_grp})
    set(rem "yes")
  endif()

  # lg_g explicitely added to the list of args
  math(EXPR lg_g_plus_p "${lg_p} + ${lg_g}")
  if(${lg_size} LESS ${lg_g_plus_p})
    set(bin "yes")
  else()
    set(bin "no")
  endif()
  
  # AND has a higher precedence in the original SH than OR so we
  # explicitly group them together here
  if( (${lg_size} LESS ${lg_kmax}) OR
      ((${lg_size} EQUAL ${lg_kmax}) AND ("${rem}" STREQUAL "no")))
    set(lg_delta_lookup ${lg_delta})
  else()
    set(lg_delta_lookup "no")
  endif()
  
  ## TODO: Formatted output maybe necessary
  file (APPEND "${output_file}"
    "    SC(  ${index}, ${lg_grp},  ${lg_delta},  ${ndelta}, ${bin},  ${lg_delta_lookup}) \\\n"
    )
  
  # Defined upon return:
  # - lg_delta_lookup (${lg_delta} or "no")
  # - bin ("yes" or "no")
  
  # Promote to PARENT_SCOPE
  set(lg_delta_lookup ${lg_delta_lookup} PARENT_SCOPE)
  set(bin ${bin} PARENT_SCOPE)
  
  # message(STATUS "size_class_result: lg_delta_lookup: ${lg_delta_lookup} bin: ${bin}")
endfunction(size_class)

####################################################################
# size_classes helper function
# Based on size_classes.sh
#
# Defined upon completion:
# - ntbins
# - nlbins
# - nbins
# - nsizes
# - lg_tiny_maxclass
# - lookup_maxclass
# - small_maxclass
# - lg_large_minclass
# - huge_maxclass
function(size_classes lg_z lg_q lg_t lg_p lg_g output_file)

  math(EXPR lg_z_plus_3 "${lg_z} + 3")
  pow2 (${lg_z_plus_3} "ptr_bits")
  pow2 (${lg_g} "g")

  file(APPEND "${output_file}"
    "#define	SIZE_CLASSES \\\n"
    "  /* index, lg_grp, lg_delta, ndelta, bin, lg_delta_lookup */ \\\n"
  )

  set(ntbins 0)
  set(nlbins 0)
  set(lg_tiny_maxclass "\"NA\"")
  set(nbins 0)

  # Tiny size classes.
  set(ndelta 0)
  set(index 0)
  set(lg_grp ${lg_t})
  set(lg_delta ${lg_grp})
  
  while(${lg_grp} LESS ${lg_q})
    # Add passing lg_g as penaltimate arg. lg_g originally passed implicitly
    # See doc for the output values
    size_class(${index} ${lg_grp} ${lg_delta} ${ndelta} ${lg_p}
      ${lg_g} ${lg_kmax} "${output_file}")
    
    if(NOT "${lg_delta_lookup}" STREQUAL "no")
      math(EXPR nlbins "${index} + 1")
    endif()
    if(NOT "${bin}" STREQUAL "no")
      math(EXPR nbins "${index} + 1")
    endif()
    math(EXPR ntbins "${ntbins} + 1")

    set(lg_tiny_maxclass ${lg_grp}) # Final written value is correct.
    math(EXPR index "${index} + 1")
    set(lg_delta ${lg_grp})
    math(EXPR lg_grp "${lg_grp} + 1")
  endwhile(${lg_grp} LESS ${lg_q})

  # First non-tiny group.
  if( ${ntbins} GREATER 0)
    file(APPEND "${output_file}"
      "                                               \\\n"
    )
    # The first size class has an unusual encoding, because the size has to be
    # split between grp and delta*ndelta.
    math(EXPR lg_grp "${lg_grp} - 1")
    set (ndelta 1)
    size_class(${index} ${lg_grp} ${lg_delta} ${ndelta} ${lg_p} ${lg_g} ${lg_kmax} "${output_file}")
    math(EXPR index "${index} + 1")
    math(EXPR lg_grp "${lg_grp} + 1")
    math(EXPR lg_delta "${lg_delta} + 1")
  endif()
  
  while (${ndelta} LESS ${g})
    size_class( ${index} ${lg_grp} ${lg_delta} ${ndelta} ${lg_p} ${lg_g} ${lg_kmax} "${output_file}")
    math(EXPR index "${index} + 1")
    math(EXPR ndelta "${ndelta} + 1")
  endwhile (${ndelta} LESS ${g})

  # All remaining groups.
  math(EXPR lg_grp "${lg_grp} + ${lg_g}")
  while(${lg_grp} LESS ${ptr_bits})

    file(APPEND "${output_file}"
    "                                         \\\n"
    )
    set(ndelta 1)

    math(EXPR ptr_bits_min1 "${ptr_bits} - 1")
    if(${lg_grp} EQUAL ${ptr_bits_min1})
      math(EXPR ndelta_limit "${g} - 1")
    else()
      set(ndelta_limit ${g})
    endif()

    while(${ndelta} LESS ${ndelta_limit} OR
          ${ndelta} EQUAL ${ndelta_limit})
          
      size_class(${index} ${lg_grp} ${lg_delta} ${ndelta} ${lg_p} ${lg_g} ${lg_kmax} "${output_file}")
      if(NOT "${lg_delta_lookup}" STREQUAL "no")
        math(EXPR nlbins "${index} + 1")
        # Final written value is correct:
        set(lookup_maxclass "((((size_t)1) << ${lg_grp}) + (((size_t)${ndelta}) << ${lg_delta}))")
      endif()
      
      if(NOT "${bin}" STREQUAL "no")
        math(EXPR nbins "${index} + 1")
        # # Final written value is correct:
        set(small_maxclass "((((size_t)1) << ${lg_grp}) + (((size_t)${ndelta}) << ${lg_delta}))")
        if( ${lg_g} GREATER 0)
          math(EXPR lg_large_minclass "${lg_grp} + 1")
        else()
          math(EXPR lg_large_minclass "${lg_grp} + 2")
        endif()
      endif()
      # Final written value is correct:
      set(huge_maxclass "((((size_t)1) << ${lg_grp}) + (((size_t)${ndelta}) << ${lg_delta}))")
      math(EXPR index "${index} + 1")
      math(EXPR ndelta "${ndelta} + 1")
    endwhile(${ndelta} LESS ${ndelta_limit} OR ${ndelta} EQUAL ${ndelta_limit})
          
    math(EXPR lg_grp "${lg_grp} + 1")
    math(EXPR lg_delta "${lg_delta} + 1")
  endwhile(${lg_grp} LESS ${ptr_bits})
  
  file(APPEND "${output_file}" "\n")
  set(nsizes ${index})

  # Defined upon completion:
  # - ntbins
  # - nlbins
  # - nbins
  # - nsizes
  # - lg_tiny_maxclass
  # - lookup_maxclass
  # - small_maxclass
  # - lg_large_minclass
  # - huge_maxclass
  
  # Promote to PARENT_SCOPE
  set(ntbins ${ntbins} PARENT_SCOPE)
  set(nlbins ${nlbins} PARENT_SCOPE)
  set(nbins ${nbins} PARENT_SCOPE)
  set(nsizes ${nsizes} PARENT_SCOPE)
  set(lg_tiny_maxclass ${lg_tiny_maxclass} PARENT_SCOPE)
  set(lookup_maxclass ${lookup_maxclass} PARENT_SCOPE)
  set(small_maxclass ${small_maxclass} PARENT_SCOPE)
  set(lg_large_minclass ${lg_large_minclass} PARENT_SCOPE)
  set(huge_maxclass ${huge_maxclass} PARENT_SCOPE)

  # message(STATUS "size_classes_result: ntbins ${ntbins} "
          # "nlbins ${nlbins} "
          # "nbins ${nbins} "
          # "nsizes ${nsizes} "
          # "lg_tiny_maxclass ${lg_tiny_maxclass} "
          # "lookup_maxclass ${lookup_maxclass} "
          # "small_maxclass ${small_maxclass} "
          # "lg_large_minclass ${lg_large_minclass} "
          # "huge_maxclass ${huge_maxclass}"
          # )

endfunction(size_classes)


###################################################
# SizeClasses
# Based on size_classes.sh
# lg_qarr - quanta
# lg_tmin - The range of tiny size classes is [2^lg_tmin..2^(lg_q-1)].
# lg_parr - list of page sizes
# lg_g - Size class group size (number of size classes for each size doubling).
function (SizeClasses lg_qarr lg_tmin lg_parr lg_g output_file)

message(STATUS "Please wait while we configure class sizes\n")

# message(STATUS "SizeClasses: lg_qarr:${lg_qarr} lg_tmin:${lg_tmin} lg_parr:${lg_parr} lg_g:${lg_g}\n"
# "output: ${output_file}"
# )

# The following limits are chosen such that they cover all supported platforms.
# Pointer sizes.
set(lg_zarr 2 3)
# Maximum lookup size.
set(lg_kmax 12)

file(WRITE "${output_file}"
"/* This file was automatically generated by size_classes.sh. */\n"
"/******************************************************************************/\n"
"#ifdef JEMALLOC_H_TYPES\n\n"
"/*\n"
" * This header requires LG_SIZEOF_PTR, LG_TINY_MIN, LG_QUANTUM, and LG_PAGE to\n"
" * be defined prior to inclusion, and it in turn defines:\n"
" *\n"
" *   LG_SIZE_CLASS_GROUP: Lg of size class count for each size doubling.\n"
" *   SIZE_CLASSES: Complete table of\n"
" *                 SC(index, lg_grp, lg_delta, ndelta, bin, lg_delta_lookup)\n"
" *                 tuples.\n"
" *     index: Size class index.\n"
" *     lg_grp: Lg group base size (no deltas added).\n"
" *     lg_delta: Lg delta to previous size class.\n"
" *     ndelta: Delta multiplier.  size == 1<<lg_grp + ndelta<<lg_delta\n"
" *     bin: 'yes' if a small bin size class, 'no' otherwise.\n"
" *     lg_delta_lookup: Same as lg_delta if a lookup table size class, 'no'\n"
" *                      otherwise.\n"
" *   NTBINS: Number of tiny bins.\n"
" *   NLBINS: Number of bins supported by the lookup table.\n"
" *   NBINS: Number of small size class bins.\n"
" *   NSIZES: Number of size classes.\n"
" *   LG_TINY_MAXCLASS: Lg of maximum tiny size class.\n"
" *   LOOKUP_MAXCLASS: Maximum size class included in lookup table.\n"
" *   SMALL_MAXCLASS: Maximum small size class.\n"
" *   LG_LARGE_MINCLASS: Lg of minimum large size class.\n"
" *   HUGE_MAXCLASS: Maximum (huge) size class.\n"
" */\n\n"
"#define	LG_SIZE_CLASS_GROUP	${lg_g}\n\n"
)

foreach(lg_z ${lg_zarr})
  foreach(lg_q ${lg_qarr})
    set(lg_t ${lg_tmin})
    while((${lg_t} LESS ${lg_q}) OR (${lg_t} EQUAL ${lg_q}))
      # Iterate through page sizes and compute how many bins there are.
      foreach(lg_p ${lg_parr})
        file(APPEND "${output_file}"
          "#if (LG_SIZEOF_PTR == ${lg_z} && LG_TINY_MIN == ${lg_t} && LG_QUANTUM == ${lg_q} && LG_PAGE == ${lg_p})\n"
        )
        size_classes(${lg_z} ${lg_q} ${lg_t} ${lg_p} ${lg_g} "${output_file}")
        file(APPEND "${output_file}"
          "#define	SIZE_CLASSES_DEFINED\n"
          "#define	NTBINS			${ntbins}\n"
          "#define	NLBINS			${nlbins}\n"
          "#define	NBINS			${nbins}\n"
          "#define	NSIZES			${nsizes}\n"
          "#define	LG_TINY_MAXCLASS	${lg_tiny_maxclass}\n"
          "#define	LOOKUP_MAXCLASS		${lookup_maxclass}\n"
          "#define	SMALL_MAXCLASS		${small_maxclass}\n"
          "#define	LG_LARGE_MINCLASS	${lg_large_minclass}\n"
          "#define	HUGE_MAXCLASS		${huge_maxclass}\n"
          "#endif\n\n"
        )
      endforeach(lg_p)
      math(EXPR lg_t "${lg_t} + 1")
    endwhile((${lg_t} LESS ${lg_q}) OR (${lg_t} EQUAL ${lg_q}))
  endforeach(lg_q in)
endforeach(lg_z)

file(APPEND "${output_file}"
"#ifndef SIZE_CLASSES_DEFINED\n"
"#  error \"No size class definitions match configuration\"\n"
"#endif\n"
"#undef SIZE_CLASSES_DEFINED\n"
"/*\n"
" * The size2index_tab lookup table uses uint8_t to encode each bin index, so we\n"
" * cannot support more than 256 small size classes.  Further constrain NBINS to\n"
" * 255 since all small size classes, plus a \"not small\" size class must be\n"
" * stored in 8 bits of arena_chunk_map_bits_t's bits field.\n"
" */\n"
"#if (NBINS > 255)\n"
"#  error \"Too many small size classes\"\n"
"#endif\n\n"
"#endif /* JEMALLOC_H_TYPES */\n"
"/******************************************************************************/\n"
"#ifdef JEMALLOC_H_STRUCTS\n\n\n"
"#endif /* JEMALLOC_H_STRUCTS */\n"
"/******************************************************************************/\n"
"#ifdef JEMALLOC_H_EXTERNS\n\n\n"
"#endif /* JEMALLOC_H_EXTERNS */\n"
"/******************************************************************************/\n"
"#ifdef JEMALLOC_H_INLINES\n\n\n"
"#endif /* JEMALLOC_H_INLINES */\n"
"/******************************************************************************/\n"
)

message(STATUS "Finished configuring class sizes\n")

endfunction (SizeClasses)

#############################################
# Generate public symbols list
function (GeneratePublicSymbolsList public_sym_list mangling_map symbol_prefix output_file)

file(REMOVE "${output_file}")

# First remove from public symbols those that appear in the mangling map
if(mangling_map)
  foreach(map_entry ${mangling_map})
    # Extract the symbol
    string(REGEX REPLACE "([^ \t]*):[^ \t]*" "\\1" sym ${map_entry})
    list(REMOVE_ITEM  public_sym_list ${sym})
    file(APPEND "${output_file}" "${map_entry}\n")
  endforeach(map_entry)
endif()  

foreach(pub_sym ${public_sym_list})
  file(APPEND "${output_file}" "${pub_sym}:${symbol_prefix}${pub_sym}\n")
endforeach(pub_sym)

endfunction(GeneratePublicSymbolsList)

#####################################################################
# Decorate symbols with a prefix
#
# This is per jemalloc_mangle.sh script.
#
# IMHO, the script has a bug that is currently reflected here
# If the public symbol as alternatively named in a mangling map it is not
# reflected here. Instead, all symbols are #defined using the passed symbol_prefix
function (GenerateJemallocMangle public_sym_list symbol_prefix output_file)

# Header
file(WRITE "${output_file}"
"/*\n * By default application code must explicitly refer to mangled symbol names,\n"
" * so that it is possible to use jemalloc in conjunction with another allocator\n"
" * in the same application.  Define JEMALLOC_MANGLE in order to cause automatic\n"
" * name mangling that matches the API prefixing that happened as a result of\n"
" * --with-mangling and/or --with-jemalloc-prefix configuration settings.\n"
" */\n"
"#ifdef JEMALLOC_MANGLE\n"
"#  ifndef JEMALLOC_NO_DEMANGLE\n"
"#    define JEMALLOC_NO_DEMANGLE\n"
"#  endif\n"
)

file(STRINGS "${public_sym_list}" INPUT_STRINGS)

foreach(line ${INPUT_STRINGS})
  string(REGEX REPLACE "([^ \t]*):[^ \t]*" "#  define \\1 ${symbol_prefix}\\1" output ${line})      
  file(APPEND "${output_file}" "${output}\n")
endforeach(line)

file(APPEND "${output_file}"
"#endif\n\n"
"/*\n"
" * The ${symbol_prefix}* macros can be used as stable alternative names for the\n"
" * public jemalloc API if JEMALLOC_NO_DEMANGLE is defined.  This is primarily\n"
" * meant for use in jemalloc itself, but it can be used by application code to\n"
" * provide isolation from the name mangling specified via --with-mangling\n"
" * and/or --with-jemalloc-prefix.\n"
" */\n"
"#ifndef JEMALLOC_NO_DEMANGLE\n"
)

foreach(line ${INPUT_STRINGS})
  string(REGEX REPLACE "([^ \t]*):[^ \t]*" "#  undef ${symbol_prefix}\\1" output ${line})      
  file(APPEND "${output_file}" "${output}\n")
endforeach(line)

# Footer
file(APPEND "${output_file}" "#endif\n")

endfunction (GenerateJemallocMangle)

########################################################################
# Generate jemalloc_rename.h per jemalloc_rename.sh
function (GenerateJemallocRename public_sym_list_file file_path)
# Header
file(WRITE "${file_path}"
  "/*\n * Name mangling for public symbols is controlled by --with-mangling and\n"
  " * --with-jemalloc-prefix.  With" "default settings the je_" "prefix is stripped by\n"
  " * these macro definitions.\n"
  " */\n#ifndef JEMALLOC_NO_RENAME\n\n"
)

file(STRINGS "${public_sym_list_file}" INPUT_STRINGS)
foreach(line ${INPUT_STRINGS})
  string(REGEX REPLACE "([^ \t]*):([^ \t]*)" "#define je_\\1 \\2" output ${line})
  file(APPEND "${file_path}" "${output}\n")
endforeach(line)

# Footer
file(APPEND "${file_path}"
  "#endif\n"
)
endfunction (GenerateJemallocRename)

###############################################################
# Create a jemalloc.h header by concatenating the following headers
# Mimic processing from jemalloc.sh
# This is a Windows specific function
function (CreateJemallocHeader header_list output_file)

file(REMOVE ${output_file})

message(STATUS "Creating public header ${output_file}")

# File Header
file(WRITE "${output_file}.header"
  "#pragma once\n\n"
  "#ifndef JEMALLOC_H_\n"
  "#define	JEMALLOC_H_\n"
  "#ifdef __cplusplus\n"
  "extern \"C\" {\n"
  "#endif\n\n"
)

# Footer
file(WRITE "${output_file}.footer"
  "#ifdef __cplusplus\n"
  "}\n"
  "#endif\n"
  "#endif /* JEMALLOC_H_ */\n"
)

# We need to utlize copy command here because of the unexpected FILE function
# behavior as follows
# - file(STRINGS) unexpectedly reads two lines when a preprocessor continuations
# is encoutered. In the process \ is replaced to a ; and we get a CMake list of
# two lines, instead of one. When such a line written back we get single line with
# ; in the middle
# - file(READ) which is expected to handle files as binaries removes trailing ; and
# ruins function prototypes.

# Create a Windows path for copy command
file(TO_NATIVE_PATH "${output_file}" ntv_output_file)
file(TO_NATIVE_PATH "${output_file}.header" ntv_header)
file(TO_NATIVE_PATH "${output_file}.footer" ntv_footer)

list(APPEND ntv_path_list ${ntv_header})

foreach(pub_hdr ${header_list} )
  set(HDR_PATH "${CMAKE_CURRENT_SOURCE_DIR}/include/jemalloc/${pub_hdr}")
  file(TO_NATIVE_PATH "${HDR_PATH}" ntv_pub_hdr)
  list(APPEND ntv_path_list +${ntv_pub_hdr})
endforeach(pub_hdr)

list(APPEND ntv_path_list +${ntv_footer})

execute_process(COMMAND $ENV{COMSPEC} /C copy ${ntv_path_list} ${ntv_output_file}
 RESULT_VARIABLE error_level
 ERROR_VARIABLE error_output)

if(NOT ${error_level} EQUAL 0)
  message(FATAL_ERROR "Public header concatenation completed with ${error_level} : ${error_output}")
endif()

file(REMOVE "${output_file}.header" "${output_file}.footer")

endfunction(CreateJemallocHeader)

############################################################################
# Redefines public symbols prefxied with je_ via a macro
# Based on public_namespace.sh which echoes the result to a stdout
function(PublicNamespace public_sym_list_file output_file)

file(REMOVE ${output_file})
file(STRINGS "${public_sym_list_file}" INPUT_STRINGS)
foreach(line ${INPUT_STRINGS})
  string(REGEX REPLACE "([^ \t]*):[^ \t]*" "#define	je_\\1 JEMALLOC_N(\\1)" output ${line})
  file(APPEND ${output_file} "${output}\n")
endforeach(line)
  
endfunction(PublicNamespace)

############################################################################
# #undefs public je_prefixed symbols
# Based on public_unnamespace.sh which echoes the result to a stdout
function(PublicUnnamespace public_sym_list_file output_file)

file(REMOVE ${output_file})
file(STRINGS "${public_sym_list_file}" INPUT_STRINGS)
foreach(line ${INPUT_STRINGS})
  string(REGEX REPLACE "([^ \t]*):[^ \t]*" "#undef	je_\\1" output ${line})
  file(APPEND ${output_file} "${output}\n")
endforeach(line)

endfunction(PublicUnnamespace)


####################################################################
# Redefines a private symbol via a macro
# Based on private_namespace.sh
function(PrivateNamespace private_sym_list_file output_file)

file(REMOVE ${output_file})
file(STRINGS ${private_sym_list_file} INPUT_STRINGS)
foreach(line ${INPUT_STRINGS})
  file(APPEND ${output_file} "#define	${line} JEMALLOC_N(${line})\n")
endforeach(line)

endfunction(PrivateNamespace)

####################################################################
# Redefines a private symbol via a macro
# Based on private_namespace.sh
function(PrivateUnnamespace private_sym_list_file output_file)

file(REMOVE ${output_file})
file(STRINGS ${private_sym_list_file} INPUT_STRINGS)
foreach(line ${INPUT_STRINGS})
  file(APPEND ${output_file} "#undef ${line}\n")
endforeach(line)

endfunction(PrivateUnnamespace)


############################################################################
# A function that configures a file_path and outputs
# end result into output_path
# ExpandDefine True/False if we want to process the file and expand
# lines that start with #undef DEFINE into what is defined in CMAKE
function (ConfigureFile file_path output_path ExpandDefine)

if(EXISTS ${file_path})
  if(NOT ${ExpandDefine})
    # Will expand only @@ macros
    configure_file(${file_path} ${output_path} @ONLY NEWLINE_STYLE WIN32) 
  else()
    # Need to Grep for ^#undef VAR lines and replace it with
    # ^#cmakedefine VAR
    file(REMOVE ${file_path}.cmake)
    file(STRINGS ${file_path} INPUT_STRINGS)
    foreach(line ${INPUT_STRINGS})
      if(${line} MATCHES "^#undef[ \t]*[^ \t]*")
        string(REGEX REPLACE "^#undef[ \t]*([^ \t]*)" "\\1" extracted_define ${line})      
        if(${extracted_define})
          file(APPEND ${file_path}.cmake "#define ${extracted_define} ${${extracted_define}}\n")
        else()
          file(APPEND ${file_path}.cmake "/* #undef ${extracted_define} */\n\n")
        endif()
      else()
        file(APPEND ${file_path}.cmake "${line}\n")
      endif()
    endforeach(line)
    configure_file(${file_path}.cmake ${output_path} @ONLY NEWLINE_STYLE WIN32)
    file(REMOVE ${file_path}.cmake)
  endif()
else()
  message(FATAL_ERROR "${file_path} not found")
endif()

endfunction(ConfigureFile)

############################################################################################
## Run Git and parse the output to populate version settings above
function (GetAndParseVersion)

if (GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(COMMAND $ENV{COMSPEC} /C 
      ${GIT_EXECUTABLE} -C ${CMAKE_CURRENT_SOURCE_DIR} describe --long --abbrev=40 HEAD OUTPUT_VARIABLE jemalloc_version)
    
    # Figure out version components    
    string (REPLACE "\n" "" jemalloc_version  ${jemalloc_version})
    set(jemalloc_version ${jemalloc_version} PARENT_SCOPE)
    message(STATUS "Version is ${jemalloc_version}")

    # replace in this order to get a valid cmake list
    string (REPLACE "-g" "-" T_VERSION ${jemalloc_version})
    string (REPLACE "-" "." T_VERSION  ${T_VERSION})
    string (REPLACE "." ";" T_VERSION  ${T_VERSION})

    list(LENGTH T_VERSION L_LEN)

    if(${L_LEN} GREATER 0)
      list(GET T_VERSION 0 jemalloc_version_major)
      set(jemalloc_version_major ${jemalloc_version_major} PARENT_SCOPE)
      message(STATUS "jemalloc_version_major: ${jemalloc_version_major}")
    endif()

    if(${L_LEN} GREATER 1)
      list(GET T_VERSION 1 jemalloc_version_minor)
      set(jemalloc_version_minor ${jemalloc_version_minor} PARENT_SCOPE)
      message(STATUS "jemalloc_version_minor: ${jemalloc_version_minor}")
    endif()

    if(${L_LEN} GREATER 2)
      list(GET T_VERSION 2 jemalloc_version_bugfix)
      set(jemalloc_version_bugfix ${jemalloc_version_bugfix} PARENT_SCOPE)
      message(STATUS "jemalloc_version_bugfix: ${jemalloc_version_bugfix}")
    endif()

    if(${L_LEN} GREATER 3)
      list(GET T_VERSION 3 jemalloc_version_nrev)
      set(jemalloc_version_nrev ${jemalloc_version_nrev} PARENT_SCOPE)
      message(STATUS "jemalloc_version_nrev: ${jemalloc_version_nrev}")
    endif()

    if(${L_LEN} GREATER 4)
      list(GET T_VERSION 4 jemalloc_version_gid)
      set(jemalloc_version_gid ${jemalloc_version_gid} PARENT_SCOPE)
      message(STATUS "jemalloc_version_gid: ${jemalloc_version_gid}")
    endif()
endif()

endfunction (GetAndParseVersion)
