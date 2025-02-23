﻿/*
 *  This plugin is intended to cleanup x86 executables (particularly XBEs for now) immediately after auto-analysis.
 *
 */

#include <ida.hpp>
#include <idp.hpp>
#include <auto.hpp>
#include <entry.hpp>
#include <bytes.hpp>
#include <loader.hpp>
#include <kernwin.hpp>
#include <typeinf.hpp>
#include <chrono>
#include <unordered_set>
#include "ida_extensions.h"

std::unordered_set<std::string> wordlist(26000);

// TODO: substitute with something else that's licensed? they're just words...
// https://github.com/dolph/dictionary/blob/master/popular.txt
void load_wordlist(std::unordered_set<std::string>& out)
{
    // build the wordlist path
    qstring plugin_dir;
    if (!get_plugin_dir(plugin_dir))
    {
        msg("Failed to load wordlist, cannot obtain plugin directory!\r\n");
        return;
    }
    plugin_dir.append("idawax_wordlist.txt");

    // open the wordlist file
    FILE* file = qfopen(plugin_dir.c_str(), "r");
    if (!file)
    {
        msg("Failed to load wordlist from '%s'\r\n", plugin_dir);
        return;
    }

    // populate the wordlist
    out.clear();
    for (qstring line; qgetline(&line, file) > 0;)
    {
        // skip small words
        if (line.size() < 4)
            continue;

        out.insert(line.c_str());
    }
}

void detect_function(const ea_t ea)
{ 
    // skip already defined functions
    func_t* func = get_func(ea);
    if (func != nullptr)
        return;

    // skip alignment and data (for now)
    flags_t flags = get_flags(ea);
    if (is_align(flags) || is_data(flags))
        return;

    msg("Creating function at 0x%X\r\n", ea);
    if (!add_func(ea))
    {
        // TODO: fixup surroundings and try again
    }
}

ea_t extend_bad_function_end(const func_t* func)
{
    int max_tries = 5;
    for (int i = 0; i < max_tries && !func_does_end(*func); i++)
    {
        // consume the next function if nothing inbetween
        func_t* next_func = get_next_func(func->start_ea);
        if (next_func->start_ea == func->end_ea && del_func(next_func->start_ea))
        {
            // TODO: remove xrefs?
            msg("Removed bad function at 0x%X\r\n", next_func->start_ea);
            del_global_name(next_func->start_ea);
            if (set_func_end(func->start_ea, next_func->end_ea))
            {
                continue;
            }
        }

        // scan until the next return/alignment/function, this isn't guaranteed to find the actual function end!
        int lookahead = 100;
        idaplace_t p = idaplace_t(func->end_ea, 0);
        insn_t ins;

        do
        {
            decode_insn(&ins, p.ea);
            p.next(NULL);
            lookahead--;
        } while (!is_func_end_insn(ins) && !is_align_insn(p.ea) && get_func(p.ea) == nullptr && lookahead > 0);    // TODO: && (!is_data(flags) && has_refs(flags))

        if (lookahead > 0 && set_func_end(func->start_ea, p.ea))
        {
            continue;
        }

        // TODO: more fix-ups

        // all attempts failed, abort
        return BADADDR;
    }

    // TODO: get_func with updated info?
    return func->end_ea;
}

// TODO: removes bad xrefs to data
void clear_bad_data_xrefs(const idaplace_t& place)
{
    // TODO: skip when dref points to a mid-instruction in a code segment (if possible)
}

void process_data(const idaplace_t& place)
{
    // only process undefined/unreferenced data
    flags_t flags = get_flags(place.ea);
    if (!(is_unknown(flags) && !has_xref(flags)))
        return;

    // get data info
    asize_t total_size = get_item_size(place.ea);
    asize_t item_size = get_data_elsize(place.ea, flags);
    bool is_array = total_size > item_size;
    asize_t array_count = total_size / item_size;
    bool is_dword_aligned = place.ea % 4 == 0;

    // TODO: not sure if mixed arrays are a thing...
    assert(total_size % item_size == 0);

    // gets potential pointer info
    ea_t offset = get_32bit(place.ea);
    flags_t offset_flags = get_flags(offset);

    // check if it points to the beginning of a function
    if (is_dword_aligned && is_func(offset_flags) && get_func(offset)->start_ea == offset)
    {
        msg("Function reference detected at 0x%X\r\n", place.ea);
        if (!create_dword(place.ea, 4))
        {
            // TODO: decompose arrays if necessary
        }
    }

    // check if it points to referenced data or defined offsets/strings
    else if (is_dword_aligned && is_data(offset_flags) && (has_xref(offset_flags) || is_strlit(offset_flags) || is_off(offset_flags, 0)))
    {
        msg("Data reference detected at 0x%X\r\n", place.ea);
        if (!create_dword(place.ea, 4))
        {
            // TODO: decompose arrays if necessary
        }
    }

    // unknown data, define based on alignment; eventually be smarter than this by peeking at xref instruction data types or surrounding data etc.
    else if (is_unknown(flags) && !is_array)
    {
        // TODO: create dword if 4-byte aligned (check for float types first), else create word if 2-byte aligned, else byte
    }
}

void process_code(const idaplace_t& place)
{
    flags_t flags;

    // skip jump tables
    if (is_jmp_table(place.ea))
        return;

    // undefined code
    if (!get_func(place.ea))
    {
        if (try_make_func_align(place.ea))
        {
            msg("Created function alignment at address 0x%X\r\n", place.ea);
        }
        else detect_function(place.ea);
    }

    func_t* func = get_func(place.ea);
    if (func != nullptr)
    {
        // skip data offsets inside functions that point to another location within the same function (undetected jump tables)
        flags = get_flags(place.ea);
        if (is_data(flags) && is_same_func(func->start_ea, get_32bit(place.ea)))
            return;

        // extend partial functions
        if (func->start_ea == place.ea && !func_does_end(*func))
        {
            msg("Found partial function at 0x%X\r\n", func->start_ea);

            ea_t orig_end = func->end_ea;
            if (extend_bad_function_end(func) > orig_end)
            {
                msg("Extended function end from 0x%X to 0x%X\r\n", orig_end, func->end_ea);
                msg_disasm_range(orig_end, func->end_ea + 1);
            }
        }

        insn_t instruction;
        decode_insn(&instruction, place.ea);

        // TODO: handle alignment within functions

        // handle undefined code within functions
        flags = get_flags(place.ea);
        if (!is_code(flags) && !is_align(flags))
            create_insn_ex(place.ea);

        remove_bad_code_xrefs(place.ea);
        detect_and_make_op_tag(instruction);
    }
}

void process_segment(const segment_t& segment)
{
    // get segment info
    qstring name;
    get_segm_name(&name, &segment);
    const char* c_name = name.c_str();

    // TODO: dyamic detection from a first-pass scan
    bool has_code = segment.type == SEG_CODE || (strstr(c_name, "BINK") && !strstr(c_name, "DATA"));
    bool has_data = segment.type == SEG_DATA || strstr(c_name, "D3D") || strstr(c_name, "DSOUND") ||
        strstr(c_name, "XNET") || strstr(c_name, "XPP") || strstr(c_name, "DOLBY") || 
        strstr(c_name, "DATA") || strstr(c_name, "$$X");

    // loop through each place address
    idaplace_t place = idaplace_t(segment.start_ea, 0);
    while (place.ea <= segment.end_ea)
    {
        if (has_code)
        {
            process_code(place);
        }
        if (has_data)
        {
            process_data(place);
        }
        place.next(NULL);
    }
}

bool idaapi run(size_t)
{
    if ( !auto_is_ok()
    && ask_yn(ASKBTN_NO,
        "HIDECANCEL\n"
        "The autoanalysis has not finished yet.\n"
        "The result might be incomplete.\n"
        "Do you want to continue?") < ASKBTN_YES )
    {
    return true;
    }

    auto start = std::chrono::high_resolution_clock::now();

    load_wordlist(wordlist);

    qvector<segment_t> segments;
    get_segments(segments);
    for (auto const& segment : segments)
    {
        process_segment(segment);
    }

    // kill any pending auto-analysis triggered by the updates we've made
    auto_cancel(0, UINT32_MAX);

    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
    msg("Cleanup finished in %d milliseconds\r\n", duration);

    return true;
}

//--------------------------------------------------------------------------
int idaapi init(void)
{
    return PLUGIN_OK;
}

//--------------------------------------------------------------------------
plugin_t PLUGIN =
{
  IDP_INTERFACE_VERSION,
  0,                    // plugin flags
  init,                 // initialize
  NULL,                 // terminate. this pointer may be NULL.
  run,                  // invoke plugin
  NULL,                 // long comment about the plugin
  NULL,                 // multiline help about the plugin
  "IDA Wax",            // the preferred short name of the plugin
  "Ctrl-F11",           // the preferred hotkey to run the plugin
};
