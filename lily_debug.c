#include "lily_impl.h"
#include "lily_symtab.h"
#include "lily_opcode.h"

static char *name_for_type(lily_val_type vt)
{
    char *mapping[] = {"builtin", "int", "str", "list", "double", "?"};
    return mapping[vt];
}

static void show_code(lily_symbol *sym)
{
    int i = 0;
    int len = sym->code_data->code_pos;
    int *code = sym->code_data->code;

    lily_symbol *target, *t2;
    int reg;

    while (i < len) {
        switch (code[i]) {
            case o_load_reg:
                target = (lily_symbol *)(code[i+2]);
                lily_impl_debugf("[%d] load          reg #%d with sym #%d\n",
                                 i, code[i+1], target->sym_id);
                i += 3;
                break;
            case o_builtin_print:
                lily_impl_debugf("[%d] builtin_print reg #%d\n", i, code[i+1]);
                i += 2;
                break;
            case o_assign:
                lily_impl_debugf("[%d] assign        reg #%d  to  reg #%d\n",
                           i, code[i+1], code[i+2]);
                i += 3;
                break;
            case o_vm_return:
                lily_impl_debugf("[%d] vm_return\n", i);
                return;
            default:
                lily_impl_debugf("[%d] bad opcode %d.\n", i, code[i]);
                return;
        }
    }
}

void lily_show_symtab(lily_symbol *symtab)
{
    lily_symbol *sym = symtab;

    while (sym != NULL) {
        char *name = sym->sym_name == NULL ? "<no-name>" : sym->sym_name;
        lily_impl_debugf("Symbol #%d (%s):\n", sym->sym_id, name);

        lily_impl_debugf("    type = %s\n", name_for_type(sym->val_type));
        if (sym->callable && sym->code_data != NULL) {
            lily_impl_debugf("    callable = yes; args = %d\n", sym->num_args);
            show_code(sym);
        }

        sym = sym->next;
    }
}