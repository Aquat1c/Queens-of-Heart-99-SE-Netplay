#ifndef QOH99_INSTRUMENT_POLICY_H
#define QOH99_INSTRUMENT_POLICY_H

static int qoh99_instr_policy_exact_opt_in(const char *value)
{
    return value != 0 && value[0] == '1' && value[1] == '\0';
}

#endif
