#include "opcodes.h"

opcode_info_t g_dotnet_opcodes[] = {
    [CEE_INVALID] = { .name = "illegal" },
#define OPDEF_REAL_OPCODES_ONLY
#define OPDEF(_cname, _sname, _pop, _push, _operand, _kind, _len, _b1, _b2, _flow) \
    [_cname] = { \
        .name = _sname, \
        .operand = OPCODE_OPERAND_##_operand, \
        .control_flow = OPCODE_CONTROL_FLOW_##_flow, \
    },
#include "metadata/opcode.def"
#undef OPDEF
#undef OPDEF_REAL_OPCODES_ONLY
};

int g_dotnet_opcodes_count = ARRAY_LEN(g_dotnet_opcodes);
STATIC_ASSERT(ARRAY_LEN(g_dotnet_opcodes) < UINT16_MAX);

uint16_t g_dotnet_opcode_lookup[] = {
#define OPDEF_REAL_OPCODES_ONLY
#define OPDEF(_cname, _sname, _pop, _push, _operand, _kind, _len, _b1, _b2, _flow) \
    [(_b1 << 8) | _b2] = _cname,
#include "metadata/opcode.def"
#undef OPDEF
#undef OPDEF_REAL_OPCODES_ONLY
};

void opcode_disasm_method(System_Reflection_MethodInfo method) {
    int offset = 0;
    System_Reflection_MethodBody body = method->MethodBody;
    System_Reflection_Assembly assembly = method->Module->Assembly;

    int i = 0;
    while (i < body->Il->Length) {
        int pc = i;
        uint16_t opcode_value = (REFPRE << 8) | body->Il->Data[i++];

        // get the actual opcode
        opcode_t opcode = g_dotnet_opcode_lookup[opcode_value];
        if (opcode == CEE_INVALID) {
            TRACE("\t\t\tIL_%04x:   illegal (%02x)", pc, opcode_value);
            continue;
        } else if (
            opcode == CEE_PREFIX1 ||
            opcode == CEE_PREFIX2 ||
            opcode == CEE_PREFIX3 ||
            opcode == CEE_PREFIX4 ||
            opcode == CEE_PREFIX5 ||
            opcode == CEE_PREFIX6 ||
            opcode == CEE_PREFIX7
        ) {
            opcode_info_t* opcode_info = &g_dotnet_opcodes[opcode];

            // setup the new prefix
            opcode_value <<= 8;
            opcode_value |= body->Il->Data[i++];
            opcode = g_dotnet_opcode_lookup[opcode_value];

            if (opcode == CEE_INVALID) {
                TRACE("\t\t\tIL_%04x:  %s.illegal (%02x)", pc, opcode_info->name, opcode_value);
                continue;
            }
        }

        // get the actual opcode
        opcode_info_t* opcode_info = &g_dotnet_opcodes[opcode];

        char param[128] = { 0 };
        switch (opcode_info->operand) {
            case OPCODE_OPERAND_InlineBrTarget: {
                int32_t value = *(int32_t*)&body->Il->Data[i];
                i += sizeof(int32_t);
                snprintf(param, sizeof(param), "IL_%04x", i + value);
            } break;
            case OPCODE_OPERAND_InlineField: {
                token_t value = *(token_t*)&body->Il->Data[i];
                i += sizeof(token_t);
                System_Reflection_FieldInfo field = assembly_get_field_by_token(assembly, value);
                snprintf(param, sizeof(param), "%U.%U::%U",
                         field->DeclaringType->Namespace, field->DeclaringType->Name, field->Name);
            } break;
            case OPCODE_OPERAND_InlineI: {
                int32_t value = *(int32_t*)&body->Il->Data[i];
                i += sizeof(int32_t);
                snprintf(param, sizeof(param), "%d", value);
            } break;
            case OPCODE_OPERAND_InlineI8: {
                int64_t value = *(int64_t*)&body->Il->Data[i];
                i += sizeof(int64_t);
                snprintf(param, sizeof(param), "%lld", value);
            } break;
            case OPCODE_OPERAND_InlineMethod: {
                token_t value = *(token_t*)&body->Il->Data[i];
                i += sizeof(token_t);
                System_Reflection_MethodInfo methodOpr = assembly_get_method_by_token(assembly, value);
                snprintf(param, sizeof(param), "%U.%U::%U",
                         methodOpr->DeclaringType->Namespace, methodOpr->DeclaringType->Name, methodOpr->Name);
            } break;
            case OPCODE_OPERAND_InlineR: i += sizeof(double); snprintf(param, sizeof(param), "<double>"); break;
            case OPCODE_OPERAND_InlineSig: i += sizeof(token_t); snprintf(param, sizeof(param), "<sig>"); break;
            case OPCODE_OPERAND_InlineString: i += sizeof(token_t); snprintf(param, sizeof(param), "<string>"); break;
            case OPCODE_OPERAND_InlineSwitch: ASSERT(!"TODO: switch support");
            case OPCODE_OPERAND_InlineTok: i += sizeof(token_t); snprintf(param, sizeof(param), "<tok>"); break;
            case OPCODE_OPERAND_InlineType: {
                token_t value = *(token_t*)&body->Il->Data[i];
                i += sizeof(token_t);
                System_Type typeOpr = assembly_get_type_by_token(assembly, value);
                snprintf(param, sizeof(param), "%U.%U", typeOpr->Namespace, typeOpr->Name);
            } break;
            case OPCODE_OPERAND_InlineVar: {
                uint16_t value = *(uint16_t*)&body->Il->Data[i];
                i += sizeof(uint16_t);
                snprintf(param, sizeof(param), "V_%u", value);
            } break;
            case OPCODE_OPERAND_ShortInlineBrTarget: {
                int8_t value = *(int8_t*)&body->Il->Data[i];
                i += sizeof(int8_t);
                snprintf(param, sizeof(param), "IL_%04x", i + value);
            } break;
            case OPCODE_OPERAND_ShortInlineI: {
                int8_t value = *(int8_t*)&body->Il->Data[i];
                i += sizeof(int8_t);
                snprintf(param, sizeof(param), "%d", value);
            } break;
            case OPCODE_OPERAND_ShortInlineR: i += sizeof(float); snprintf(param, sizeof(param), "<float>"); break;
            case OPCODE_OPERAND_ShortInlineVar: {
                uint8_t value = *(uint8_t*)&body->Il->Data[i];
                i += sizeof(uint8_t);
                snprintf(param, sizeof(param), "V_%u", value);
            } break;
            default: break;
        }

        TRACE("\t\t\tIL_%04x:  %s %s", pc, opcode_info->name, param);
    }
}