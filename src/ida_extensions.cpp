#include "ida_extensions.h"
#include <ida.hpp>
#include <idp.hpp>
#include <auto.hpp>
#include <entry.hpp>
#include <bytes.hpp>
#include <loader.hpp>
#include <kernwin.hpp>
#include <typeinf.hpp>

#pragma region Instructions

bool is_jxx_insn(const insn_t& ins)
{
    return ins.get_canon_mnem()[0] == 'j';
}

bool is_jxx_insn(const ea_t ea)
{
    insn_t ins;
    return decode_insn(&ins, ea) && is_jxx_insn(ins);
}

bool is_jcc_insn(const insn_t& ins)
{
    const char* mnem = ins.get_canon_mnem();
    return mnem[0] == 'j' && mnem[1] != 'm';
}

bool is_jcc_insn(const ea_t ea)
{
    insn_t ins;
    return decode_insn(&ins, ea) && is_jcc_insn(ins);
}

bool is_jmp_insn(const insn_t& ins)
{
    return !strcmp(ins.get_canon_mnem(), "jmp");
}

bool is_jmp_insn(const ea_t ea)
{
    insn_t ins;
    return decode_insn(&ins, ea) && is_jmp_insn(ins);
}

bool is_jmp_table(const ea_t ea)
{
    flags_t flags = get_flags(ea);
    if (!is_data(flags) || !has_cmt(flags) || !has_xref(flags))
        return false;

    // TODO: has to be a better/quicker way to do this; get_X_dref -> get_switch_info?
    qstring cmt;
    get_cmt(&cmt, ea, false);
    return strstr(cmt.c_str(), "table for switch");
}

bool is_retn_insn(const insn_t& ins)
{
    const char* mnem = ins.get_canon_mnem();
    return strstr(mnem, "ret") == mnem;
}

bool is_retn_insn(const ea_t ea)
{
    insn_t ins;
    return decode_insn(&ins, ea) && is_retn_insn(ins);
}

#pragma endregion

#pragma region Functions

void get_func_end_insn(const func_t& func, insn_t& out)
{
    idaplace_t end_place = idaplace_t(func.end_ea, 0);
    end_place.prev(NULL);
    decode_insn(&out, end_place.ea);
}

bool is_func_truncated(const func_t& func)
{
    // get last instruction
    insn_t end_insn;
    get_func_end_insn(func, end_insn);
    
    // return true if it doesn't end on a ret or jmp or TODO: int 3
    // NOTE: do not use func_t::does_return() because it returns true if *any* part of the function returns!
    // NOTE: do not use is_ret_insn because it's buggy!
    return !is_retn_insn(end_insn) && !is_jmp_insn(end_insn.ea);
}

#pragma endregion

#pragma region Segments

void get_segments(qvector<segment_t>& out)
{
    segment_t* seg = get_first_seg();
    while (seg != nullptr)
    {
        out.push_back(*seg);
        seg = get_next_seg(seg->start_ea);
    }
}

#pragma endregion

#pragma region XRefs

void del_dref_ex(const ea_t from, const ea_t to)
{
    // TODO: confirm it exists?

    // remove the reference
    del_dref(from, to);
    msg("Removed bad xref at address 0x%X to 0x%X\r\n", from, to);

    // repair referencing instruction (or data) by converting bad offsets to number types
    flags_t flags = get_flags(from);
    if (is_code(flags))
    {
        insn_t ins;
        decode_insn(&ins, from);

        if (is_off0(flags) && (ins.ops[0].addr == to || ins.ops[0].value == to))
        {
            op_num(from, 0);
        }
        else if (is_off1(flags) && (ins.ops[1].addr == to || ins.ops[1].value == to))
        {
            op_num(from, 1);
        }
    }
    else // if (is_off0(flags))
    {
        // TODO: additional checks to pick apart references within arrays?
        op_num(from, 0);
    }
}

void del_all_drefs_to(const ea_t ea)
{
    // get first source reference
    ea_t dref = get_first_dref_to(ea);
    if (dref == BADADDR)
        return;

    while (dref != BADADDR)
    {
        del_dref_ex(dref, ea);

        // get next source reference
        dref = get_next_dref_to(ea, dref);
    }
}

void del_all_drefs_to(const ea_t start, const ea_t end)
{
    // TODO: iterate through places rather than ea's
    for (int i = start; i < end; i++)
    {
        del_all_drefs_to(i);
    }
}

#pragma endregion

#pragma region Code Analysis

void remove_bad_code_xrefs(const ea_t ea)
{
    flags_t flags = get_flags(ea);

    // skip data xrefs
    if (is_data(flags))
        return;

    // skip addresses without any references
    if (!has_xref(flags))
        return;

    // skip function xrefs for now, they're probably good
    func_t* func = get_func(ea);
    if (func != nullptr && ea == func->start_ea)
    {
        // TODO: be skeptical of references to functions that are 4K aligned, except segment starts
        return;
    }

    // skip jump tables
    if (is_jmp_table(ea))
        return;

    // loop through all references
    ea_t dref = get_first_dref_to(ea);
    while (dref != BADADDR)
    {
        // skip internal xrefs
        if (is_same_func(ea, dref))
            goto next;

        // skip xrefs that come from struct members (exception scopetable entries, etc.)
        flags_t dref_flags = get_flags(dref);
        if (is_struct(dref_flags))
            goto next;

        // skip switch statement xrefs
        if (get_switch_parent(ea) != BADADDR)
            goto next;

        // TODO: only decode if the dref is part of a code segment (or range)
        insn_t i;
        if (decode_insn(&i, dref))
        {
            // skip jump refs
            if (is_jxx_insn(i))
                goto next;
        }

        // remove bad xref
        del_dref_ex(dref, ea);

    next:
        // get next source reference
        dref = get_next_dref_to(ea, dref);
    }
}

void detect_and_make_op_tag(const insn_t& instruction)
{
    // TODO: additional operand logic, onyl works with first one currently

    // check if value is a sensical string
    int val = swap32(instruction.ops[0].value);
    char* tag = (char*)&val;
    int i = 0;
    for (; i < 4 && (isalnum(tag[i]) || ispunct(tag[i])); i++);

    if (i == 4)
    {
        // check for float comment hint
        qstring cmt;
        get_cmt(&cmt, instruction.ea, false);
        if (!strcmp(cmt.c_str(), "float"))
            return;

        // check for absolute float values between 0.0001 and 10000
        float fval = fabs(*(float*)&instruction.ops[0].value);
        if (fval > 0.0001 && fval < 10000)
            return;

        // set operand to character type
        op_chr(instruction.ea, 0);
        
        // log the tag
        msg("Found '%.4s' tag at 0x%X\r\n", tag, instruction.ea);
    }
}

#pragma endregion

#pragma region Data Analysis

bool is_align16(const ea_t ea)
{
    flags_t flags = get_flags(ea);
    func_t* func = get_func(ea);

    // skip alignment within functions
    if (func != nullptr)
        return false;

    // skip pre-existing 16-byte alignment
    if (is_align(flags) && get_alignment(ea) == 4)
        return false;

    // skip already-aligned instances
    if ((ea & 0xF) == 0)
        return false;

    // look for NOPs until the next 16-byte alignment
    ea_t aligned = (ea + 0xF) & 0xFFFFFFF0;
    ea_t pos = ea;
    insn_t ins;
    do
    {
        // skip invalid instructions
        int size = decode_insn(&ins, pos);
        if (size == 0)
            return false;

        // skip non-alignment
        if (!is_align_insn(pos))
            return false;

        // skip jump tables
        if (is_jmp_table(pos))
            return false;

        pos += size;
    } while (pos < aligned);

    // return true if the aligned instruction is not also an alignment instruction
    return pos == aligned && !is_align_insn(pos);
}

void detect_and_make_align(const ea_t ea)    // TODO: rename to detect_func_align and return status on whether or not it was detected?
{    
    if (is_align16(ea))
    {
        msg("Creating alignment at address 0x%X\r\n", ea);

        // next 16 - byte alignment
        ea_t aligned = (ea + 0xF) & 0xFFFFFFF0;
        size_t padding_size = aligned - ea;
        if (!create_align(ea, padding_size, 4))
        {
            // undefine the entire range of padding
            del_all_drefs_to(ea, aligned);
            del_items(ea, DELIT_EXPAND, padding_size);
            if (!create_align(ea, padding_size, 4))
            {
                // TODO: log hard failure
            }
        }
    }
}

#pragma endregion

#pragma region Display

void msg_disasm_range(ea_t start, ea_t end, bool indent, bool no_addresses, bool only_instructions, bool no_comments, bool no_empty)
{
    // backup the current view type
    tcc_renderer_type_t orig_view = get_view_renderer_type(get_current_viewer());

    // TODO: there has to be a better way to generate disassembly text in a more efficient and universal manner than swapping views?
    // idaplace_t::generate() is a less flexible option, might have to implement printing manually for more control
    set_view_renderer_type(get_current_viewer(), no_addresses ? TCCRT_GRAPH : TCCRT_FLAT);

    // loop through disassembly text
    text_t text;
    gen_disasm_text(text, start, end, false);   // NOTE: this renders differently based on the current view!
    for (auto& line : text)
    {
        // only print the default line if desired
        if (only_instructions && !line.is_default)
            continue;

        // remove formatting
        qstring stripped;
        tag_remove(&stripped, line.line);
        char* str = (char*)stripped.c_str();

        // remove comments if desired
        if (no_comments)
        {
            // scan until comment found, and if it exists...
            char* c = strchr(str, ';');
            if (c)
            {
                // reverse-scan until first non-whitespace or beginning of string
                while (c > str && isspace(c[-1]))
                {
                    c--;
                }

                // null terminate
                *c = 0;
            }
        }

        // skip empty lines if desired
        if (no_empty && strlen(str) == 0)
            continue;

        // print to console
        msg((indent ? "\t%s\r\n" : "%s\r\n"), + str);
    }

    // restore the original view type
    set_view_renderer_type(get_current_viewer(), orig_view);
}

#pragma endregion

