#pragma once

#include <dotnet/dotnet.h>
#include <util/except.h>

/**
 * Fill the field from the given field signature
 *
 * @param sig       [IN] The signature
 * @param assembly  [IN] The assembly this field is related to
 * @param field     [IN] The field
 */
err_t sig_parse_field(const uint8_t* sig, assembly_t assembly, field_info_t field);

/**
 * Fill the method's locals information from the given local var signature
 *
 * @param sig       [IN] The signature
 * @param method    [IN] The method
 */
err_t sig_parse_method_locals(const uint8_t* sig, method_info_t method);

/**
 * Fill the method from the given method signature
 *
 * @param sig       [IN] The signature
 * @param method    [IN] The method
 */
err_t sig_parse_method(const uint8_t* sig, method_info_t method);
