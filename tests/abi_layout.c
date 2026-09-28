#include "recomp.h"
#include "librecomp/sections.h"
#include <stddef.h>
#include <stdio.h>
int main(void) {
    printf("{\"context_size\":%zu,\"context_align\":%zu,\"gpr_size\":%zu,\"r29\":%zu,\"r31\":%zu,\"f0\":%zu,\"f_odd\":%zu,\"mips3_float_mode\":%zu,\"FuncEntry\":%zu,\"RelocEntry\":%zu,\"SectionTableEntry\":%zu,\"section_index\":%zu}\n",
        sizeof(recomp_context), _Alignof(recomp_context), sizeof(gpr),
        offsetof(recomp_context,r29), offsetof(recomp_context,r31), offsetof(recomp_context,f0),
        offsetof(recomp_context,f_odd), offsetof(recomp_context,mips3_float_mode),
        sizeof(FuncEntry),sizeof(RelocEntry),sizeof(SectionTableEntry),offsetof(SectionTableEntry,index));
    return 0;
}
