#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "lily_alloc.h"
#include "lily_impl.h"
#include "lily_value.h"
#include "lily_opcode.h"
#include "lily_vm.h"
#include "lily_bind.h"
#include "lily_parser.h"
#include "lily_seed.h"

#include "lily_cls_any.h"
#include "lily_cls_hash.h"
#include "lily_cls_function.h"
#include "lily_cls_list.h"
#include "lily_cls_string.h"

extern uint64_t siphash24(const void *src, unsigned long src_sz, const char key[16]);

#define INTEGER_OP(OP) \
lhs_reg = vm_regs[code[code_pos + 2]]; \
rhs_reg = vm_regs[code[code_pos + 3]]; \
vm_regs[code[code_pos+4]]->value.integer = \
lhs_reg->value.integer OP rhs_reg->value.integer; \
vm_regs[code[code_pos+4]]->flags = VAL_IS_PRIMITIVE; \
code_pos += 5;

#define INTDBL_OP(OP) \
lhs_reg = vm_regs[code[code_pos + 2]]; \
rhs_reg = vm_regs[code[code_pos + 3]]; \
if (lhs_reg->type == double_type) { \
    if (rhs_reg->type == double_type) \
        vm_regs[code[code_pos+4]]->value.doubleval = \
        lhs_reg->value.doubleval OP rhs_reg->value.doubleval; \
    else \
        vm_regs[code[code_pos+4]]->value.doubleval = \
        lhs_reg->value.doubleval OP rhs_reg->value.integer; \
} \
else \
    vm_regs[code[code_pos+4]]->value.doubleval = \
    lhs_reg->value.integer OP rhs_reg->value.doubleval; \
vm_regs[code[code_pos+4]]->flags = VAL_IS_PRIMITIVE; \
code_pos += 5;

/* EQUALITY_COMPARE_OP is used for == and !=, instead of a normal COMPARE_OP.
   The difference is that this will allow op on any type, so long as the lhs
   and rhs agree on the full type. This allows comparing functions, hashes
   lists, and more.

   Arguments are:
   * op:       The operation to perform relative to the values given. This will
               be substituted like: lhs->value OP rhs->value
               This is done for everything BUT string.
   * stringop: The operation to perform relative to the result of strcmp. ==
               does == 0, as an example. */
#define EQUALITY_COMPARE_OP(OP, STRINGOP) \
lhs_reg = vm_regs[code[code_pos + 2]]; \
rhs_reg = vm_regs[code[code_pos + 3]]; \
if (lhs_reg->type == double_type) { \
    if (rhs_reg->type == double_type) \
        vm_regs[code[code_pos+4]]->value.integer = \
        (lhs_reg->value.doubleval OP rhs_reg->value.doubleval); \
    else \
        vm_regs[code[code_pos+4]]->value.integer = \
        (lhs_reg->value.doubleval OP rhs_reg->value.integer); \
} \
else if (lhs_reg->type == integer_type) { \
    if (rhs_reg->type == integer_type) \
        vm_regs[code[code_pos+4]]->value.integer =  \
        (lhs_reg->value.integer OP rhs_reg->value.integer); \
    else \
        vm_regs[code[code_pos+4]]->value.integer = \
        (lhs_reg->value.integer OP rhs_reg->value.doubleval); \
} \
else if (lhs_reg->type->cls->id == SYM_CLASS_STRING) { \
    vm_regs[code[code_pos+4]]->value.integer = \
    strcmp(lhs_reg->value.string->string, \
           rhs_reg->value.string->string) STRINGOP; \
} \
else if (lhs_reg->type == rhs_reg->type) { \
    vm_regs[code[code_pos+4]]->value.integer = \
    compare_values(vm, lhs_reg, rhs_reg) OP 1; \
} \
vm_regs[code[code_pos+4]]->flags = VAL_IS_PRIMITIVE; \
code_pos += 5;

#define COMPARE_OP(OP, STRINGOP) \
lhs_reg = vm_regs[code[code_pos + 2]]; \
rhs_reg = vm_regs[code[code_pos + 3]]; \
if (lhs_reg->type == double_type) { \
    if (rhs_reg->type == double_type) \
        vm_regs[code[code_pos+4]]->value.integer = \
        (lhs_reg->value.doubleval OP rhs_reg->value.doubleval); \
    else \
        vm_regs[code[code_pos+4]]->value.integer = \
        (lhs_reg->value.doubleval OP rhs_reg->value.integer); \
} \
else if (lhs_reg->type == integer_type) { \
    if (rhs_reg->type == integer_type) \
        vm_regs[code[code_pos+4]]->value.integer =  \
        (lhs_reg->value.integer OP rhs_reg->value.integer); \
    else \
        vm_regs[code[code_pos+4]]->value.integer = \
        (lhs_reg->value.integer OP rhs_reg->value.doubleval); \
} \
else if (lhs_reg->type->cls->id == SYM_CLASS_STRING) { \
    vm_regs[code[code_pos+4]]->value.integer = \
    strcmp(lhs_reg->value.string->string, \
           rhs_reg->value.string->string) STRINGOP; \
} \
vm_regs[code[code_pos+4]]->flags = VAL_IS_PRIMITIVE; \
code_pos += 5;

/** If you're interested in working on the vm, or having trouble with it, here's
    some advice that might make things easier.

    * The vm uses a mix of refcounting and a gc. Values that are tagged by the
      type maker or symtab as being possibly circular get a gc tag. Anything
      else can only be destroyed by pure ref/deref. This allows Lily to have a
      gc, but allow it to mark and sweep over fewer values.

    * lily_value.h contains useful functions for handling values. In general, it
      is an unwise idea to manipulate values directly.

    * Forgetting to increase a ref typically shows itself through invalid reads
      and/or writes during garbage collection.

    * -g is used to set the number of gc tags allowed at once. If Lily crashes
      at a certain number, then a value is missing a gc tag.

    * Foreign functions can cache vm->vm_regs (most all do). Do not ever use
      a cached value of vm->vm_regs after lily_foreign_call, as the registers
      may have been resized. **/

/* This demands some explanation. A foreign function takes three arguments: The
   vm, a number of arguments, and the arguments themselves. There are times when
   a foreign function wants to call another foreign function such as
   ```["a", "b", "c"].apply(string::upper)```

   The calling function (list::map) has one register set aside for
   string::upper's result. string::upper needs a value, and needs to return to
   somewhere.

   Emitter writes down function calls in the form of '#values, return, args...'
   In the above case, vm_regs[0] is where the return is, and the one argument is
   at [1]. If there were two arguments, then the second argument would be at
   [2]. The third at [3], etc.

   This allows passing a maximum of 64 values (including the return). 64 values
   should be enough for everyone. :) */
static uint16_t foreign_call_stack[] = {0,
     1,  2,  3,  4,  5,  6,  7,  8,  9, 10,
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
    31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50,
    51, 52, 53, 54, 55, 56, 57, 58, 59, 60,
    61, 62, 63, 64};

/***
 *      ____       _
 *     / ___|  ___| |_ _   _ _ __
 *     \___ \ / _ \ __| | | | '_ \
 *      ___) |  __/ |_| |_| | |_) |
 *     |____/ \___|\__|\__,_| .__/
 *                          |_|
 */

static void add_call_frame(lily_vm_state *);
static void invoke_gc(lily_vm_state *);

lily_vm_state *lily_new_vm_state(lily_options *options,
        lily_raiser *raiser)
{
    lily_vm_state *vm = lily_malloc(sizeof(lily_vm_state));
    vm->data = options->data;
    vm->gc_threshold = options->gc_threshold;

    /* todo: This is a terrible, horrible key to use. Make a better one using
             some randomness or...something. Just not this. */
    char sipkey[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf};
    lily_vm_catch_entry *catch_entry = lily_malloc(sizeof(lily_vm_catch_entry));

    vm->sipkey = lily_malloc(16);
    vm->foreign_code = lily_malloc(sizeof(uint16_t));
    vm->call_depth = 0;
    vm->raiser = raiser;
    vm->vm_regs = NULL;
    vm->regs_from_main = NULL;
    vm->num_registers = 0;
    vm->offset_max_registers = 0;
    vm->true_max_registers = 0;
    vm->gc_live_entries = NULL;
    vm->gc_spare_entries = NULL;
    vm->gc_live_entry_count = 0;
    vm->gc_pass = 0;
    vm->prep_id_start = 0;
    vm->catch_chain = NULL;
    vm->catch_top = NULL;
    vm->symtab = NULL;
    vm->readonly_table = NULL;
    vm->readonly_count = 0;
    vm->call_chain = NULL;
    vm->vm_list = lily_malloc(sizeof(lily_vm_list));
    vm->vm_list->values = lily_malloc(4 * sizeof(lily_value *));
    vm->vm_list->pos = 0;
    vm->vm_list->size = 4;

    add_call_frame(vm);

    memcpy(vm->sipkey, sipkey, 16);

    vm->catch_chain = catch_entry;
    catch_entry->prev = NULL;
    catch_entry->next = NULL;

    vm->foreign_code[0] = o_return_from_vm;

    return vm;
}

static void destroy_gc_entries(lily_vm_state *vm)
{
    lily_gc_entry *gc_iter, *gc_temp;
    gc_iter = vm->gc_live_entries;

    while (gc_iter != NULL) {
        gc_temp = gc_iter->next;

        lily_free(gc_iter);

        gc_iter = gc_temp;
    }

    gc_iter = vm->gc_spare_entries;
    while (gc_iter != NULL) {
        gc_temp = gc_iter->next;

        lily_free(gc_iter);

        gc_iter = gc_temp;
    }
}

void lily_free_vm(lily_vm_state *vm)
{
    lily_value **regs_from_main = vm->regs_from_main;
    lily_value *reg;
    int i;
    if (vm->catch_chain != NULL) {
        while (vm->catch_chain->prev)
            vm->catch_chain = vm->catch_chain->prev;

        lily_vm_catch_entry *catch_iter = vm->catch_chain;
        lily_vm_catch_entry *catch_next;
        while (catch_iter) {
            catch_next = catch_iter->next;
            lily_free(catch_iter);
            catch_iter = catch_next;
        }
    }

    for (i = vm->true_max_registers-1;i >= 0;i--) {
        reg = regs_from_main[i];

        lily_deref(reg);

        lily_free(reg);
    }

    /* This keeps the final gc invoke from touching the now-deleted registers.
       It also ensures the last invoke will get everything. */
    vm->num_registers = 0;
    vm->offset_max_registers = 0;
    vm->true_max_registers = 0;

    lily_free(regs_from_main);

    lily_call_frame *frame_iter = vm->call_chain;
    lily_call_frame *frame_next;

    while (frame_iter->prev)
        frame_iter = frame_iter->prev;

    while (frame_iter) {
        frame_next = frame_iter->next;
        lily_free(frame_iter);
        frame_iter = frame_next;
    }

    /* vm->num_registers is now 0, so this will sweep everything. */
    invoke_gc(vm);
    destroy_gc_entries(vm);

    lily_free(vm->vm_list->values);
    lily_free(vm->vm_list);
    lily_free(vm->readonly_table);
    lily_free(vm->foreign_code);
    lily_free(vm->sipkey);
    lily_free(vm);
}

/***
 *       ____    ____
 *      / ___|  / ___|
 *     | |  _  | |
 *     | |_| | | |___
 *      \____|  \____|
 *
 */

/* This is Lily's garbage collector. It runs in multiple stages:
   1: Go to each _in-use_ register that is not nil and use the appropriate
      gc_marker call to mark all values inside that value which are visible.
      Visible items are set to the vm's ->gc_pass.
   2: Go through all the gc items now. Anything which doesn't have the current
      pass as its last_pass is considered unreachable. This will deref values
      that cannot be circular, or forcibly collect possibly-circular values.
      Caveats:
      * Some gc_entries may have their value set to 0/NULL. This happens when
        a possibly-circular value has been deleted through typical ref/deref
        means.
      * The lily_gc_collect_* will collect everything inside a non-circular
        value, but not the value itself. It will set last_pass to -1 when it
        does this. This is necessary because it's possible that a value may be
        sent to lily_gc_collect_* calls multiple times (because circular
        references). If it's deleted, then there will be invalid reads.
   3: Stage 1 skipped registers that are not in-use, because Lily just hasn't
      gotten around to clearing them yet. However, some of those registers may
      contain a value that has a gc_entry that indicates that the value is to be
      destroyed. It's -very- important that these registers be marked as nil so
      that prep_registers will not try to deref a value that has been destroyed
      by the gc.
   4: Finally, destroy the lists, anys, etc. that stage 2 didn't clear.
      Absolutely nothing is using these now, so it's safe to destroy them. */
static void invoke_gc(lily_vm_state *vm)
{
    /* This is (sort of) a mark-and-sweep garbage collector. This is called when
       a certain number of allocations have been done. Take note that values
       can be destroyed by deref. However, those values will have the gc_entry's
       value set to NULL as an indicator. */
    vm->gc_pass++;

    lily_value **regs_from_main = vm->regs_from_main;
    int pass = vm->gc_pass;
    int num_registers = vm->num_registers;
    int i;
    lily_gc_entry *gc_iter;

    /* Stage 1: Go through all registers and use the appropriate gc_marker call
                that will mark every inner value that's visible. */
    for (i = 0;i < vm->num_registers;i++) {
        lily_value *reg = regs_from_main[i];
        if ((reg->type->flags & TYPE_MAYBE_CIRCULAR) &&
            (reg->flags & VAL_IS_NIL) == 0 &&
             reg->value.gc_generic->gc_entry != NULL) {
            (*reg->type->cls->gc_marker)(pass, reg);
        }
    }

    /* Stage 2: Start destroying everything that wasn't marked as visible.
                Don't forget to check ->value for NULL in case the value was
                destroyed through normal ref/deref means. */
    for (gc_iter = vm->gc_live_entries;
         gc_iter;
         gc_iter = gc_iter->next) {
        if (gc_iter->last_pass != pass &&
            gc_iter->value.generic != NULL) {
            lily_gc_collect_value(gc_iter->value_type,
                    gc_iter->value);
        }
    }

    /* num_registers is -1 if the vm is calling this from lily_free_vm_state and
       there are no registers left. */
    if (num_registers != -1) {
        int i;
        /* Stage 3: Check registers not currently in use to see if they hold a
                    value that's going to be collected. If so, then mark the
                    register as nil so that the value will be cleared later. */
        for (i = vm->num_registers;i < vm->true_max_registers;i++) {
            lily_value *reg = regs_from_main[i];
            if ((reg->type->flags & TYPE_MAYBE_CIRCULAR) &&
                (reg->flags & VAL_IS_NIL) == 0 &&
                /* Not sure if this next line is necessary though... */
                reg->value.gc_generic->gc_entry != NULL &&
                reg->value.gc_generic->gc_entry->last_pass == -1) {
                reg->flags |= VAL_IS_NIL;
            }
        }
    }

    /* Stage 4: Delete the lists/anys/etc. that stage 2 didn't delete.
                Nothing is using them anymore. Also, sort entries into those
                that are living and those that are no longer used. */
    i = 0;
    lily_gc_entry *new_live_entries = NULL;
    lily_gc_entry *new_spare_entries = vm->gc_spare_entries;
    lily_gc_entry *iter_next = NULL;
    gc_iter = vm->gc_live_entries;

    while (gc_iter) {
        iter_next = gc_iter->next;
        i++;

        if (gc_iter->last_pass == -1) {
            lily_free(gc_iter->value.generic);

            gc_iter->next = new_spare_entries;
            new_spare_entries = gc_iter;
        }
        else {
            gc_iter->next = new_live_entries;
            new_live_entries = gc_iter;
        }

        gc_iter = iter_next;
    }

    vm->gc_live_entry_count = i;
    vm->gc_live_entries = new_live_entries;
    vm->gc_spare_entries = new_spare_entries;
}

/* This will attempt to grab a spare entry and associate it with the value
   given. If there are no spare entries, then a new entry is made. These entries
   are how the gc is able to locate values later.

   If the number of living gc objects is at or past the threshold, then the
   collector will run BEFORE the association. This is intentional, as 'value' is
   not guaranteed to be in a register. */
void lily_add_gc_item(lily_vm_state *vm, lily_type *value_type,
        lily_generic_gc_val *value)
{
    if (vm->gc_live_entry_count >= vm->gc_threshold)
        invoke_gc(vm);

    lily_gc_entry *new_entry;
    if (vm->gc_spare_entries != NULL) {
        new_entry = vm->gc_spare_entries;
        vm->gc_spare_entries = vm->gc_spare_entries->next;
    }
    else
        new_entry = lily_malloc(sizeof(lily_gc_entry));

    new_entry->value_type = value_type;
    new_entry->value.gc_generic = value;
    new_entry->last_pass = 0;

    new_entry->next = vm->gc_live_entries;
    vm->gc_live_entries = new_entry;

    /* Attach the gc_entry to the value so the caller doesn't have to. */
    value->gc_entry = new_entry;
    vm->gc_live_entry_count++;
}

/***
 *      ____            _     _
 *     |  _ \ ___  __ _(_)___| |_ ___ _ __ ___
 *     | |_) / _ \/ _` | / __| __/ _ \ '__/ __|
 *     |  _ <  __/ (_| | \__ \ ||  __/ |  \__ \
 *     |_| \_\___|\__, |_|___/\__\___|_|  |___/
 *                |___/
 */

/** Lily is a register-based vm. This means that each call has a block of values
    that belong to it. Upon a call's entry, the types of the registers are set,
    and values are put into the registers. Each register has a type that it will
    retain through the lifetime of the call.

    This section deals with operations concerning registers. One area that is
    moderately difficult is handling generics. Lily does not create specialized
    concretely-typed functions in place of generic ones, but instead checks for
    soundness and defers things to vm-time. The vm is then tasked with changing
    a register with a seed type of, say, A, into whatever it should be for the
    given invocation. **/

/* This function ensures that 'register_need' more registers will be available.
   This may resize (and thus invalidate) vm->regs_from_main and vm->vm_regs. */
static void grow_vm_registers(lily_vm_state *vm, int register_need)
{
    /* This is so that there will be a couple of registers after
       vm->offset_max_registers. The vm uses this when doing foreign function
       calls. */
    register_need += 2;

    lily_value **new_regs;
    lily_type *integer_type = vm->integer_type;
    int i = vm->true_max_registers;

    ptrdiff_t reg_offset = vm->vm_regs - vm->regs_from_main;

    /* Size is zero only when this is called the first time and no registers
       have been made available. */
    int size = i;
    if (size == 0)
        size = 1;

    do
        size *= 2;
    while (size < register_need);

    /* Remember, use regs_from_main, NOT vm_regs, which is likely adjusted. */
    new_regs = lily_realloc(vm->regs_from_main, size *
            sizeof(lily_value *));

    /* Realloc can move the pointer, so always recalculate vm_regs again using
       regs_from_main and the offset. */
    vm->regs_from_main = new_regs;
    vm->vm_regs = new_regs + reg_offset;

    /* Start creating new registers. Have them default to an integer type so that
       nothing has to check for a NULL type. Integer is used as the default
       because it is not ref'd. */
    for (;i < size;i++)
        new_regs[i] = lily_new_value(VAL_IS_NIL | VAL_IS_PRIMITIVE, integer_type,
            (lily_raw_value){.integer = 0});

    vm->true_max_registers = size;
    vm->offset_max_registers = size - 2;
}

/* This is called when there are unsolved generic-typed registers during prep.
   This function makes use of ts, the arguments passed, and maybe upvalues to
   solve what the generic types are. Register types are initialized to proper
   concrete types. */
static void resolve_generic_registers(lily_vm_state *vm,
        lily_function_val *fval, int args_collected)
{
    int save_ceiling = lily_ts_raise_ceiling(vm->ts);
    int i;
    lily_value **target_regs = vm->regs_from_main + vm->num_registers;

    lily_register_info *ri = fval->reg_info;

    for (i = 0;i < args_collected;i++) {
        lily_type *left_type = ri[i].type;
        lily_type *right_type = target_regs[i]->type;

        lily_ts_pull_generics(vm->ts, left_type, right_type);
    }

    if (fval->type_block_spot != 0) {
        /* There are some variables in the closure with types that can
           contribute toward the solving. */
        lily_type **types = vm->type_block->types->data;
        uint16_t *spots = vm->type_block->spots->data;
        int i;

        for (i = fval->type_block_spot - 1;types[i];i++)
            lily_ts_pull_generics(vm->ts, types[i], fval->upvalues[spots[i]]->type);
    }

    /* All generics SHOULD now have types (if they don't, it's a bug and they'll
       default to any). Start figuring out what the real types are. */
    int reg_stop = fval->reg_count;
    for (;i < reg_stop;i++) {
        lily_type *new_type = ri[i].type;
        if (new_type->flags & TYPE_IS_UNRESOLVED)
            new_type = lily_ts_resolve(vm->ts, new_type);

        lily_value *reg = target_regs[i];

        lily_deref(reg);

        reg->flags = VAL_IS_NIL | (new_type->cls->flags & VAL_IS_PRIMITIVE);
        reg->type = new_type;
    }

    lily_ts_lower_ceiling(vm->ts, save_ceiling);
}

/* This is called to fix the types of registers that aren't parameters. */
static inline void fix_register_types(lily_vm_state *vm,
        lily_function_val *fval, int args_collected)
{
    if (fval->has_generics == 0) {
        lily_value **target_regs = vm->regs_from_main + vm->num_registers;
        lily_register_info *register_seeds = fval->reg_info;
        /* For the rest of the registers, clear whatever value they have. */
        for (;args_collected < fval->reg_count;args_collected++) {
            lily_register_info seed = register_seeds[args_collected];

            lily_value *reg = target_regs[args_collected];
            lily_deref(reg);

            /* These are now just nil. Nothing more, nothing less. */
            reg->flags = VAL_IS_NIL;
            reg->type = seed.type;
        }
    }
    else
        resolve_generic_registers(vm, fval, args_collected);
}

/* This is called to initialize the registers that 'fval' will need to types
   that it expects. Old values are given a deref. Parameters are copied over and
   given refs. */
static void prep_registers(lily_vm_state *vm, lily_function_val *fval,
        uint16_t *code)
{
    int register_need = vm->num_registers + fval->reg_count;
    int i;
    lily_value **input_regs = vm->vm_regs;
    lily_value **target_regs = vm->regs_from_main + vm->num_registers;

    /* A function's args always come first, so copy arguments over while clearing
       old values. */
    for (i = 0;i < code[4];i++) {
        lily_value *get_reg = input_regs[code[6+i]];
        lily_value *set_reg = target_regs[i];

        if ((get_reg->flags & VAL_IS_NOT_DEREFABLE) == 0)
            get_reg->value.generic->refcount++;

        if ((set_reg->flags & VAL_IS_NOT_DEREFABLE) == 0)
            lily_deref(set_reg);

        *set_reg = *get_reg;
    }

    if (i != fval->reg_count)
        fix_register_types(vm, fval, i);

    vm->num_registers = register_need;
}

/***
 *      _   _      _
 *     | | | | ___| |_ __   ___ _ __ ___
 *     | |_| |/ _ \ | '_ \ / _ \ '__/ __|
 *     |  _  |  __/ | |_) |  __/ |  \__ \
 *     |_| |_|\___|_| .__/ \___|_|  |___/
 *                  |_|
 */

static void add_call_frame(lily_vm_state *vm)
{
    lily_call_frame *new_frame = lily_malloc(sizeof(lily_call_frame));

    /* This intentionally doesn't set anything but prev and next because the
       caller will have proper values for those. */
    new_frame->prev = vm->call_chain;
    new_frame->next = NULL;
    new_frame->return_target = NULL;

    if (vm->call_chain != NULL)
        vm->call_chain->next = new_frame;

    vm->call_chain = new_frame;
}

/* This determines if 'left' is equivalent to 'right' by recursively calling an
   equality function on them. This may return RuntimeError if it encounters an
   recursive value. */
static int compare_values(lily_vm_state *vm, lily_value *left, lily_value *right)
{
    class_eq_func eq_func = left->type->cls->eq_func;
    int depth = 0;

    int result = eq_func(vm, &depth, left, right);
    return result;
}

static void add_catch_entry(lily_vm_state *vm)
{
    lily_vm_catch_entry *new_entry = lily_malloc(sizeof(lily_vm_catch_entry));

    vm->catch_chain->next = new_entry;
    new_entry->next = NULL;
    new_entry->prev = vm->catch_chain;
}

/***
 *      _____
 *     | ____|_ __ _ __ ___  _ __ ___
 *     |  _| | '__| '__/ _ \| '__/ __|
 *     | |___| |  | | | (_) | |  \__ \
 *     |_____|_|  |_|  \___/|_|  |___/
 *
 */

/* Raise KeyError with 'key' as the value of the message. */
static void key_error(lily_vm_state *vm, int code_pos, lily_value *key)
{
    vm->call_chain->line_num = vm->call_chain->code[code_pos + 1];

    lily_raise(vm->raiser, lily_KeyError, "^V\n", key);
}

/* Raise IndexError, noting that 'bad_index' is, well, bad. */
static void boundary_error(lily_vm_state *vm, int bad_index)
{
    lily_raise(vm->raiser, lily_IndexError,
            "Subscript index %d is out of range.\n", bad_index);
}

/***
 *      ____        _ _ _   _
 *     | __ ) _   _(_) | |_(_)_ __  ___
 *     |  _ \| | | | | | __| | '_ \/ __|
 *     | |_) | |_| | | | |_| | | | \__ \
 *     |____/ \__,_|_|_|\__|_|_| |_|___/
 *
 */

static lily_list_val *build_traceback_raw(lily_vm_state *);

void lily_builtin_calltrace(lily_vm_state *vm, uint16_t argc, uint16_t *code)
{
    lily_value *result = vm->vm_regs[code[0]];

    /* Nobody is going to care that the most recent function is calltrace, so
       omit that. */
    vm->call_chain = vm->call_chain->prev;
    vm->call_depth--;

    lily_list_val *traceback_val = build_traceback_raw(vm);

    vm->call_chain = vm->call_chain->next;
    vm->call_depth++;

    lily_raw_value v = {.list = traceback_val};
    lily_move_raw_value(result, v);
}

void lily_builtin_print(lily_vm_state *vm, uint16_t argc, uint16_t *code)
{
    lily_value *reg = vm->vm_regs[code[1]];
    if (reg->type->cls->id == SYM_CLASS_STRING)
        lily_impl_puts(vm->data, reg->value.string->string);
    else {
        lily_msgbuf *msgbuf = vm->vm_buffer;
        lily_msgbuf_flush(msgbuf);
        lily_msgbuf_add_value(msgbuf, reg);
        lily_impl_puts(vm->data, msgbuf->message);
    }

    lily_impl_puts(vm->data, "\n");
}

void lily_process_format_string(lily_vm_state *vm, uint16_t *code)
{
    char *fmt;
    int arg_pos, fmt_index;
    lily_list_val *vararg_lv;
    lily_value **vm_regs = vm->vm_regs;
    lily_msgbuf *vm_buffer = vm->vm_buffer;
    lily_msgbuf_flush(vm_buffer);

    fmt = vm_regs[code[1]]->value.string->string;
    vararg_lv = vm_regs[code[2]]->value.list;
    arg_pos = 0;
    fmt_index = 0;
    int text_start = 0;
    int text_stop = 0;

    while (1) {
        char ch = fmt[fmt_index];

        if (ch != '%') {
            if (ch == '\0')
                break;

            text_stop++;
        }
        else if (ch == '%') {
            if (arg_pos == vararg_lv->num_values)
                lily_raise(vm->raiser, lily_FormatError,
                        "Not enough args for printfmt.\n");

            lily_msgbuf_add_text_range(vm_buffer, fmt, text_start, text_stop);
            text_start = fmt_index + 2;
            text_stop = text_start;

            fmt_index++;

            lily_value *arg = vararg_lv->elems[arg_pos]->value.any->inner_value;
            int cls_id = arg->type->cls->id;
            lily_raw_value val = arg->value;

            if (fmt[fmt_index] == 'd') {
                if (cls_id != SYM_CLASS_INTEGER)
                    lily_raise(vm->raiser, lily_FormatError,
                            "%%d is not valid for type ^T.\n",
                            arg->type);

                lily_msgbuf_add_int(vm_buffer, val.integer);
            }
            else if (fmt[fmt_index] == 's') {
                if (cls_id != SYM_CLASS_STRING)
                    lily_msgbuf_add_value(vm_buffer, arg);
                else
                    lily_msgbuf_add(vm_buffer, val.string->string);
            }
            else if (fmt[fmt_index] == 'f') {
                if (cls_id != SYM_CLASS_DOUBLE)
                    lily_raise(vm->raiser, lily_FormatError,
                            "%%f is not valid for type ^T.\n",
                            arg->type);

                lily_msgbuf_add_double(vm_buffer, val.doubleval);
            }
            arg_pos++;
        }
        fmt_index++;
    }

    lily_msgbuf_add_text_range(vm_buffer, fmt, text_start, text_stop);
}

void lily_builtin_printfmt(lily_vm_state *vm, uint16_t argc, uint16_t *code)
{
    lily_process_format_string(vm, code);
    lily_impl_puts(vm->data, vm->vm_buffer->message);
}

/***
 *       ___                      _
 *      / _ \ _ __   ___ ___   __| | ___  ___
 *     | | | | '_ \ / __/ _ \ / _` |/ _ \/ __|
 *     | |_| | |_) | (_| (_) | (_| |  __/\__ \
 *      \___/| .__/ \___\___/ \__,_|\___||___/
 *           |_|
 */

/** These functions handle various opcodes for the vm. The thinking is to try to
    keep the vm exec function "small" by kicking out big things. **/

/* This handles assignments that target either 'any' or an enum. The right side
   may or may not be an any/enum. */
static void do_o_box_assign(lily_vm_state *vm, lily_value *lhs_reg,
        lily_value *rhs_reg)
{
    if (rhs_reg->type->cls->id == SYM_CLASS_ANY &&
        lhs_reg->type == rhs_reg->type)
        /* This special case is necessary because it prevents an any from being
           accidentally placed within an 'any'. This can happen if the left is
           actually an 'any', and the right is a generic that became an 'any'.
           The emitter won't know if, say, A is going to be 'any' or not, so it
           writes a box assign as a precaution. */
        lily_assign_value(lhs_reg, rhs_reg);
    else {
        if ((rhs_reg->flags & VAL_IS_NOT_DEREFABLE) == 0)
            rhs_reg->value.generic->refcount++;

        lily_any_val *lhs_any = lily_new_any_val();

        if (lhs_reg->type->flags & TYPE_MAYBE_CIRCULAR)
            lily_add_gc_item(vm, lhs_reg->type, (lily_generic_gc_val *)lhs_any);

        *(lhs_any->inner_value) = *rhs_reg;
        lily_move_raw_value(lhs_reg, (lily_raw_value)lhs_any);
    }
}

/* Internally, classes are really just tuples. So assigning them is like
   accessing a tuple, except that the index is a raw int instead of needing to
   be loaded from a register. */
static void do_o_set_property(lily_vm_state *vm, uint16_t *code, int code_pos)
{
    lily_value **vm_regs = vm->vm_regs;
    lily_value *rhs_reg;
    int index;
    lily_instance_val *ival;

    ival = vm_regs[code[code_pos + 2]]->value.instance;
    index = code[code_pos + 3];
    rhs_reg = vm_regs[code[code_pos + 4]];

    lily_assign_value(ival->values[index], rhs_reg);
}

static void do_o_get_property(lily_vm_state *vm, uint16_t *code, int code_pos)
{
    lily_value **vm_regs = vm->vm_regs;
    lily_value *result_reg;
    int index;
    lily_instance_val *ival;

    ival = vm_regs[code[code_pos + 2]]->value.instance;
    index = code[code_pos + 3];
    result_reg = vm_regs[code[code_pos + 4]];

    lily_assign_value(result_reg, ival->values[index]);
}

/* This handles subscript assignment. The index is a register, and needs to be
   validated. */
static void do_o_set_item(lily_vm_state *vm, uint16_t *code, int code_pos)
{
    lily_value **vm_regs = vm->vm_regs;
    lily_value *lhs_reg, *index_reg, *rhs_reg;

    lhs_reg = vm_regs[code[code_pos + 2]];
    index_reg = vm_regs[code[code_pos + 3]];
    rhs_reg = vm_regs[code[code_pos + 4]];

    if (lhs_reg->type->cls->id != SYM_CLASS_HASH) {
        lily_list_val *list_val = lhs_reg->value.list;
        int index_int = index_reg->value.integer;

        if (index_int < 0) {
            int new_index = list_val->num_values + index_int;
            if (new_index < 0)
                boundary_error(vm, index_int);

            index_int = new_index;
        }
        else if (index_int >= list_val->num_values)
            boundary_error(vm, index_int);

        lily_assign_value(list_val->elems[index_int], rhs_reg);
    }
    else
        lily_hash_set_elem(vm, lhs_reg->value.hash, index_reg, rhs_reg);
}

/* This handles subscript access. The index is a register, and needs to be
   validated. */
static void do_o_get_item(lily_vm_state *vm, uint16_t *code, int code_pos)
{
    lily_value **vm_regs = vm->vm_regs;
    lily_value *lhs_reg, *index_reg, *result_reg;

    lhs_reg = vm_regs[code[code_pos + 2]];
    index_reg = vm_regs[code[code_pos + 3]];
    result_reg = vm_regs[code[code_pos + 4]];

    /* list and tuple have the same representation internally. Since list
       stores proper values, lily_assign_value automagically set the type to
       the right thing. */
    if (lhs_reg->type->cls->id == SYM_CLASS_STRING)
        lily_string_subscript(vm, lhs_reg, index_reg, result_reg);
    else if (lhs_reg->type->cls->id != SYM_CLASS_HASH) {
        lily_list_val *list_val = lhs_reg->value.list;
        int index_int = index_reg->value.integer;

        if (index_int < 0) {
            int new_index = list_val->num_values + index_int;
            if (new_index < 0)
                boundary_error(vm, index_int);

            index_int = new_index;
        }
        else if (index_int >= list_val->num_values)
            boundary_error(vm, index_int);

        lily_assign_value(result_reg, list_val->elems[index_int]);
    }
    else {
        lily_hash_elem *hash_elem = lily_hash_get_elem(vm, lhs_reg->value.hash,
                index_reg);

        /* Give up if the key doesn't exist. */
        if (hash_elem == NULL)
            key_error(vm, code_pos, index_reg);

        lily_assign_value(result_reg, hash_elem->elem_value);
    }
}

/* This builds a hash. It's written like '#pairs, key, value, key, value...'. */
static void do_o_build_hash(lily_vm_state *vm, uint16_t *code, int code_pos)
{
    lily_value **vm_regs = vm->vm_regs;
    int i, num_values;
    lily_value *result, *key_reg, *value_reg;

    num_values = (intptr_t)(code[code_pos + 2]);
    result = vm_regs[code[code_pos + 3 + num_values]];

    lily_hash_val *hash_val = lily_new_hash_val();

    if ((result->type->flags & TYPE_MAYBE_CIRCULAR))
        lily_add_gc_item(vm, result->type, (lily_generic_gc_val *)hash_val);

    lily_deref(result);

    result->value.hash = hash_val;
    result->flags = 0;

    for (i = 0;
         i < num_values;
         i += 2) {
        key_reg = vm_regs[code[code_pos + 3 + i]];
        value_reg = vm_regs[code[code_pos + 3 + i + 1]];

        lily_hash_set_elem(vm, hash_val, key_reg, value_reg);
    }
}

/* Lists and tuples are effectively the same thing internally, since the list
   value holds proper values. This is used primarily to do as the name suggests.
   However, variant types are also tuples (but with a different name). */
static void do_o_build_list_tuple(lily_vm_state *vm, uint16_t *code)
{
    lily_value **vm_regs = vm->vm_regs;
    int num_elems = (intptr_t)(code[2]);
    lily_value *result = vm_regs[code[3+num_elems]];

    lily_list_val *lv = lily_new_list_val();
    lily_value **elems = lily_malloc(num_elems * sizeof(lily_value *));

    lv->num_values = num_elems;
    lv->elems = elems;

    if ((result->type->flags & TYPE_MAYBE_CIRCULAR))
        lily_add_gc_item(vm, result->type, (lily_generic_gc_val *)lv);

    lily_raw_value v = {.list = lv};
    lily_move_raw_value(result, v);

    int i;
    for (i = 0;i < num_elems;i++) {
        lily_value *rhs_reg = vm_regs[code[3+i]];
        elems[i] = lily_copy_value(rhs_reg);
    }
}

static void do_o_build_enum(lily_vm_state *vm, uint16_t *code)
{
    lily_value **vm_regs = vm->vm_regs;
    int variant_id = code[2];
    int num_values = code[3];
    lily_value *result = vm_regs[code[code[3] + 4]];
    int slots_needed = result->type->cls->enum_slot_count;

    lily_instance_val *ival = lily_new_instance_val();
    lily_value **slots = lily_malloc(slots_needed * sizeof(lily_value *));
    ival->num_values = num_values;
    ival->values = slots;
    ival->variant_id = variant_id;

    if ((result->type->flags & TYPE_MAYBE_CIRCULAR))
        lily_add_gc_item(vm, result->type, (lily_generic_gc_val *)ival);

    lily_raw_value v = {.instance = ival};
    lily_move_raw_value(result, v);

    int i;
    for (i = 0;i < num_values;i++) {
        lily_value *rhs_reg = vm_regs[code[4+i]];
        slots[i] = lily_copy_value(rhs_reg);
    }
}

/* This raises a user-defined exception. The emitter has verified that the thing
   to be raised is raiseable (extends Exception). */
static void do_o_raise(lily_vm_state *vm, lily_value *exception_val)
{
    /* The Exception class has values[0] as the message, values[1] as the
       container for traceback. */

    lily_instance_val *ival = exception_val->value.instance;
    char *message = ival->values[0]->value.string->string;

    lily_raise_value(vm->raiser, exception_val, message);
}

/* o_setup_optargs is a strange opcode. The contents are
   '#values, lit, reg, lit, reg...'. This takes advantage of VAL_IS_NIL to check
   if a value needs to be set on something. */
static void do_o_setup_optargs(lily_vm_state *vm, uint16_t *code, int code_pos)
{
    lily_value **vm_regs = vm->vm_regs;
    lily_tie **ro_table = vm->readonly_table;

    /* This goes in reverse because arguments fill from the bottom up. Doing it
       this way means that the loop can stop when it finds the first filled
       argument. It's a slight optimization, but such an easy one. */
    int count = code[code_pos + 1];
    int i = count + code_pos + 1;
    int half = count / 2;
    int end = i - half;

    for (;i > end;i--) {
        lily_value *left = vm_regs[code[i]];
        if ((left->flags & VAL_IS_NIL) == 0)
            break;

        /* Note! The right side is ALWAYS a literal. Do not use vm_regs! */
        lily_tie *right = ro_table[code[i - half]];

        /* The left side has been verified to be nil, so there is no reason for
           a deref check.
           The right side is a literal, so it is definitely marked protected,
           and there is no need to check to ref it. */
        left->flags = right->flags;
        left->value = right->value;
    }
}

/* This creates a new instance of a class. This checks if the current call is
   part of a constructor chain. If so, it will attempt to use the value
   currently being built instead of making a new one. */
static void do_o_new_instance(lily_vm_state *vm, uint16_t *code)
{
    int i, total_entries;
    lily_value **vm_regs = vm->vm_regs;
    lily_value *result = vm_regs[code[2]];
    lily_class *instance_class = result->type->cls;

    total_entries = instance_class->prop_count;

    lily_call_frame *caller_frame = vm->call_chain->prev;

    /* Check to see if the caller is in the process of building a subclass
       of this value. If that is the case, then use that instance instead of
       building one that will simply be tossed. */
    if (caller_frame->build_value &&
        caller_frame->build_value->true_type->cls->id > instance_class->id) {

        result->value.instance = caller_frame->build_value;
        result->value.generic->refcount++;
        result->flags = 0;

        /* Important! This allows this memory-saving trick to bubble up through
           multiple ::new calls! */
        vm->call_chain->build_value = caller_frame->build_value;
        return;
    }

    lily_instance_val *iv = lily_malloc(sizeof(lily_instance_val));
    lily_value **iv_values = lily_malloc(total_entries * sizeof(lily_value *));

    iv->num_values = -1;
    iv->refcount = 1;
    iv->values = iv_values;
    iv->gc_entry = NULL;
    iv->true_type = result->type;

    if ((result->type->flags & TYPE_MAYBE_CIRCULAR))
        lily_add_gc_item(vm, result->type, (lily_generic_gc_val *)iv);

    lily_deref(result);

    result->value.instance = iv;
    result->flags = 0;

    i = 0;

    lily_class *prop_class = instance_class;
    lily_prop_entry *prop = instance_class->properties;
    for (i = total_entries - 1;i >= 0;i--, prop = prop->next) {
        /* If the properties of this class run out, then grab more from the
           superclass. This is single-inheritance, so the properties ARE there
           somewhere. */
        while (prop == NULL) {
            prop_class = prop_class->parent;
            prop = prop_class->properties;
        }

        lily_type *value_type = prop->type;
        if (value_type->flags & TYPE_IS_UNRESOLVED)
            value_type = lily_ts_resolve_by_second(vm->ts, result->type,
                    value_type);

        iv->values[i] = lily_new_value(VAL_IS_NIL, value_type,
                (lily_raw_value){.integer = 0});
    }

    iv->num_values = total_entries;

    /* This is set so that a superclass ::new can simply pull this instance,
       since this instance will have >= the # of types. */
    vm->call_chain->build_value = iv;
}

/***
 *       ____ _
 *      / ___| | ___  ___ _   _ _ __ ___  ___
 *     | |   | |/ _ \/ __| | | | '__/ _ \/ __|
 *     | |___| | (_) \__ \ |_| | | |  __/\__ \
 *      \____|_|\___/|___/\__,_|_|  \___||___/
 *
 */

/** Closures are pretty easy in the vm. It helps that the emitter has written
    'mirroring' get/set upvalue instructions around closed values. That means
    that the closure's information will always be up-to-date.

    Closures in the vm work by first creating a shallow copy of a given function
    value. The closure will then create an area for closure values. These
    values are termed the closure's cells.

    A closure is permitted to share cells with another closure. A cell will be
    destroyed when it has a zero for the cell refcount. This prevents the value
    from being destroyed too early.

    Each opcode that initializes closure data is responsible for returning the
    cells that it made. This returned data is used by lily_vm_execute to do
    closure get/set but without having to fetch the closure each time. It's not
    strictly necessary, but it's a performance boost. **/

/* This takes a value and makes a closure cell that is a copy of that value. The
   value is given a ref increase. */
static lily_value *make_cell_from(lily_value *value)
{
    lily_value *result = lily_malloc(sizeof(lily_value));
    *result = *value;
    result->cell_refcount = 1;
    if ((value->flags & VAL_IS_NOT_DEREFABLE) == 0)
        value->value.generic->refcount++;

    return result;
}

/* This opcode is the bottom level of closure creation. It is responsible for
   creating the original closure. */
static lily_value **do_o_create_closure(lily_vm_state *vm, uint16_t *code)
{
    int count = code[2];
    lily_value *result = vm->vm_regs[code[3]];

    lily_function_val *last_call = vm->call_chain->function;

    lily_function_val *closure_func = lily_new_function_copy(last_call);
    /* Without complete type information, it's hard to know if the closure
       really needs a tag. Better safe than not. */
    lily_add_gc_item(vm, result->type, (lily_generic_gc_val *)closure_func);

    lily_value **upvalues = lily_malloc(sizeof(lily_value *) * count);

    /* Cells are initially NULL so that o_set_upvalue knows to copy a new value
       into a cell. */
    int i;
    for (i = 0;i < count;i++)
        upvalues[i] = NULL;

    closure_func->num_upvalues = count;
    closure_func->upvalues = upvalues;
    closure_func->refcount = 1;

    lily_raw_value v = {.function = closure_func};
    lily_move_raw_value(result, v);
    return upvalues;
}

/* This copies cells from 'source' to 'target'. Cells that exist are given a
   cell_refcount bump. */
static void copy_upvalues(lily_function_val *target, lily_function_val *source)
{
    lily_value **source_upvalues = source->upvalues;
    int count = source->num_upvalues;

    lily_value **new_upvalues = lily_malloc(sizeof(lily_value *) * count);
    lily_value *up;
    int i;

    for (i = 0;i < count;i++) {
        up = source_upvalues[i];
        if (up)
            up->cell_refcount++;

        new_upvalues[i] = up;
    }

    target->upvalues = new_upvalues;
    target->num_upvalues = count;
}

/* This opcode will create a copy of a given function that pulls upvalues from
   the specified closure. */
static void do_o_create_function(lily_vm_state *vm, uint16_t *code)
{
    lily_value **vm_regs = vm->vm_regs;
    lily_value *input_closure_reg = vm_regs[code[1]];

    lily_tie *target_literal = vm->readonly_table[code[2]];
    lily_function_val *target_func = target_literal->value.function;

    lily_value *result_reg = vm_regs[code[3]];
    lily_function_val *new_closure = lily_new_function_copy(target_func);
    new_closure->refcount = 1;

    lily_add_gc_item(vm, target_literal->type,
            (lily_generic_gc_val *)new_closure);

    copy_upvalues(new_closure, input_closure_reg->value.function);

    lily_raw_value v = {.function = new_closure};
    lily_move_raw_value(result_reg, v);
    new_closure->refcount++;
}

/* This is written at the top of a define that uses a closure (unless that
   define is a class method).

   This instruction is unique in that there's a particular problem that needs to
   be addressed. If function 'f' is a closure and is recursively called, there
   will be existing cells at the level of 'f'. Naturally, this will lead to the
   cells at that level being rewritten.

   Would you expect calling a function recursively to modify local values in the
   current frame? Almost certainly not! This solves that problem by including
   the spots in the closure at the level of 'f'. These spots are deref'd and
   NULL'd, so that any recursive call does not damage locals. */
static lily_value **do_o_load_closure(lily_vm_state *vm, uint16_t *code)
{
    lily_function_val *input_closure = vm->call_chain->function;

    lily_value **upvalues = input_closure->upvalues;
    int count = code[2];
    int i;
    lily_value *up;

    code = code + 3;

    for (i = 0;i < count;i++) {
        up = upvalues[code[i]];
        if (up) {
            up->cell_refcount--;
            if (up->cell_refcount == 0) {
                lily_deref(up);
                lily_free(up);
            }

            upvalues[code[i]] = NULL;
        }
    }

    lily_value *result_reg = vm->vm_regs[code[i]];
    lily_raw_value v = {.function = input_closure};

    /* This does move+manual refcount increase because there's no proper Lily
       value here (and f will always be refcounted). */
    lily_move_raw_value(result_reg, v);
    input_closure->refcount++;

    return input_closure->upvalues;
}

/* This handles when a class method is a closure. Class methods will pull
   closure information from a special *closure property in the class. Doing it
   that way allows class methods to be used statically with ease regardless of
   if they are a closure. */
static lily_value **do_o_load_class_closure(lily_vm_state *vm, uint16_t *code,
        int code_pos)
{
    do_o_get_property(vm, code, code_pos);
    lily_value *result_reg = vm->vm_regs[code[code_pos + 4]];
    lily_function_val *input_closure = result_reg->value.function;

    lily_function_val *new_closure = lily_new_function_copy(input_closure);
    new_closure->refcount = 1;
    copy_upvalues(new_closure, input_closure);

    lily_raw_value v = {.function = new_closure};
    lily_move_raw_value(result_reg, v);

    return new_closure->upvalues;
}

/***
 *      _____                    _   _
 *     | ____|_  _____ ___ _ __ | |_(_) ___  _ __  ___
 *     |  _| \ \/ / __/ _ \ '_ \| __| |/ _ \| '_ \/ __|
 *     | |___ >  < (_|  __/ |_) | |_| | (_) | | | \__ \
 *     |_____/_/\_\___\___| .__/ \__|_|\___/|_| |_|___/
 *                        |_|
 */

/** Exception capture is a small but important part of the vm. Exception
    capturing can be thought of as two parts: One, trying to build trace, and
    two, trying to catch the exception. The first of those is relatively easy.

    Actually capturing exceptions is a little rough though. The interpreter
    currently allows raising a code that the vm's exception capture later has to
    possibly dynaload (eww). **/

/* This builds the current exception traceback into a raw list value. It is up
   to the caller to move the raw list to somewhere useful. */
static lily_list_val *build_traceback_raw(lily_vm_state *vm)
{
    lily_type *string_type = vm->symtab->string_class->type;
    lily_list_val *lv = lily_new_list_val();

    lv->elems = lily_malloc(vm->call_depth * sizeof(lily_value *));
    lv->num_values = -1;
    lily_call_frame *frame_iter;

    int i;

    /* The call chain goes from the most recent to least. Work around that by
       allocating elements in reverse order. It's safe to do this because
       nothing in this loop can trigger the gc. */
    for (i = vm->call_depth, frame_iter = vm->call_chain;
         i >= 1;
         i--, frame_iter = frame_iter->prev) {
        lily_function_val *func_val = frame_iter->function;
        char *path;
        char line[16] = "";
        char *class_name;
        char *separator;
        char *name = func_val->trace_name;
        if (func_val->code) {
            path = func_val->import->path;
            sprintf(line, "%d:", frame_iter->line_num);
        }
        else
            path = "[C]";

        if (func_val->class_name == NULL) {
            class_name = "";
            separator = "";
        }
        else {
            separator = "::";
            class_name = func_val->class_name;
        }

        /* +15 accounts for there maybe being a separator, the non-%s text, and
           maybe having a line number. */
        int str_size = strlen(class_name) + strlen(path) + strlen(line) + 15;

        lily_string_val *sv = lily_malloc(sizeof(lily_string_val));
        char *str = lily_malloc(str_size);
        int real_size = sprintf(str, "%s:%s from %s%s%s", path, line,
                class_name, separator, name);
        sv->refcount = 1;
        sv->size = real_size;
        sv->string = str;

        lv->elems[i - 1] = lily_new_value(0, string_type,
                (lily_raw_value){.string = sv});
    }

    lv->num_values = vm->call_depth;
    return lv;
}

/* This acts as a wrapper over build_traceback_raw. The raw list_val is put into
   a proper lily_value. */
static lily_value *build_traceback(lily_vm_state *vm)
{
    lily_list_val *lv = build_traceback_raw(vm);
    return lily_new_value(0, vm->traceback_type, (lily_raw_value){.list = lv});
}

/* This is called when a builtin exception has been thrown. All builtin
   exceptions are subclasses of Exception with only a traceback and message
   field being set. This builds a new value of the given type with the message
   and newly-made traceback. */
static void make_proper_exception_val(lily_vm_state *vm,
        lily_type *raised_type, lily_value *result)
{
    lily_instance_val *ival = lily_malloc(sizeof(lily_instance_val));

    ival->values = lily_malloc(2 * sizeof(lily_value *));
    ival->num_values = -1;
    ival->refcount = 1;
    ival->gc_entry = NULL;
    ival->true_type = raised_type;

    lily_value *message_val = lily_bind_string(vm->symtab,
            vm->raiser->msgbuf->message);

    lily_msgbuf_flush(vm->raiser->msgbuf);
    ival->values[0] = message_val;
    ival->num_values = 1;

    lily_value *traceback_val = build_traceback(vm);

    ival->values[1] = traceback_val;
    ival->num_values = 2;

    lily_raw_value v = {.instance = ival};
    lily_move_raw_value(result, v);
}

/* This is called when 'raise' raises an error. The traceback property is
   assigned to freshly-made traceback. The other fields of the value are left
   intact, however. */
static void fixup_exception_val(lily_vm_state *vm, lily_value *result,
        lily_value *thrown)
{
    lily_assign_value(result, thrown);
    lily_list_val *raw_trace = build_traceback_raw(vm);
    lily_instance_val *iv = result->value.instance;
    lily_raw_value v = {.list = raw_trace};

    /* Only things that inherit from Exception are can be raised, and Exception
       always has the traceback at slot 1. It's safe to do this. */
    lily_move_raw_value(iv->values[1], v);
}

/* This attempts to catch the exception that the raiser currently holds. If it
   succeeds, then the vm's state is updated and the exception is cleared out.

   This function will refuse to catch exceptions that are not on the current
   internal lily_vm_execute depth. This is so that the vm will return out. To do
   otherwise leaves the vm thinking it is N levels deep but being, say, N - 2
   levels deep.

   Returns 1 if the exception has been caught, 0 otherwise. */
static int maybe_catch_exception(lily_vm_state *vm)
{
    lily_type *raised_type = vm->raiser->exception_type;

    if (vm->catch_top == NULL)
        return 0;

    lily_jump_link *raiser_jump = vm->raiser->all_jumps;
    lily_class *raised_cls;

    if (raised_type == NULL) {
        const char *except_name = lily_name_for_error(vm->raiser);
        /* TODO: The vm should not be doing this HERE, because multiple levels
           of the interpreter have to deal with this. Provide something so that
           exceptions raised at vm-time have to do the dynaload somewhere that
           is not here. */
        raised_cls = lily_maybe_dynaload_class(vm->parser, NULL, except_name);
    }
    else
        raised_cls = raised_type->cls;

    lily_vm_catch_entry *catch_iter = vm->catch_top;
    lily_value *catch_reg = NULL;
    lily_value **stack_regs;
    int do_unbox, jump_location, match;

    match = 0;

    while (catch_iter != NULL) {
        /* It's extremely important that the vm not attempt to catch exceptions
           that were not made in the same jump level. If it does, the vm could
           be called from a foreign function, but think it isn't. */
        if (catch_iter->jump_entry != raiser_jump) {
            vm->catch_top = catch_iter;
            break;
        }

        lily_call_frame *call_frame = catch_iter->call_frame;
        uint16_t *code = call_frame->function->code;
        /* A try block is done when the next jump is at 0 (because 0 would
           always be going back, which is illogical otherwise). */
        jump_location = code[catch_iter->code_pos];
        stack_regs = vm->regs_from_main + catch_iter->offset_from_main;

        while (jump_location != 0) {
            /* Instead of the vm hopping around to different o_except blocks,
               this function visits them to find out which (if any) handles
               the current exception.
               +1 is the line number (for debug), +2 is the spot for the next
               jump, and +3 is a register that holds a value to store this
               particular exception. */
            int next_location = code[jump_location + 2];
            catch_reg = stack_regs[code[jump_location + 4]];
            lily_type *catch_type = catch_reg->type;
            if (lily_class_greater_eq(catch_type->cls, raised_cls)) {
                /* ...So that execution resumes from within the except block. */
                do_unbox = code[jump_location + 3];
                jump_location += 5;
                match = 1;
                break;
            }

            jump_location = next_location;
        }

        if (match)
            break;

        catch_iter = catch_iter->prev;
    }

    if (match) {
        if (do_unbox) {
            /* There is a var that the exception needs to be dropped into. If
               this exception was triggered by raise, then use that (after
               dumping traceback into it). If not, create a new instance to
               hold the info. */
            lily_value *raised_value = vm->raiser->exception_value;
            if (raised_value)
                fixup_exception_val(vm, catch_reg, raised_value);
            else
                make_proper_exception_val(vm, raised_type, catch_reg);
        }

        vm->raiser->exception_value = NULL;
        vm->call_chain = catch_iter->call_frame;
        vm->call_depth = catch_iter->call_frame_depth;
        vm->vm_list->pos = catch_iter->vm_list_pos;
        vm->vm_regs = stack_regs;
        vm->call_chain->code_pos = jump_location;
        /* Each try block can only successfully handle one exception, so use
           ->prev to prevent using the same block again. */
        vm->catch_top = catch_iter->prev;
        if (vm->catch_top != NULL)
            vm->catch_chain = vm->catch_top;
        else
            vm->catch_chain = catch_iter;
    }

    return match;
}

/***
 *      _____              _                  _    ____ ___
 *     |  ___|__  _ __ ___(_) __ _ _ __      / \  |  _ \_ _|
 *     | |_ / _ \| '__/ _ \ |/ _` | '_ \    / _ \ | |_) | |
 *     |  _| (_) | | |  __/ | (_| | | | |  / ___ \|  __/| |
 *     |_|  \___/|_|  \___|_|\__, |_| |_| /_/   \_\_|  |___|
 *                           |___/
 */

/** Foreign functions that are looking to interact with the interpreter can use
    the functions within here. Do be careful with foreign calls, however. **/


/* This executes 'call_val' using however many values are provided. The call's
   result should be 'expect_type'. If the call does not return a value, then
   'expect_type' can be NULL. 'cached' is used to determine if some extra setup
   is necessary (the first call has to setup frames and upward types). The
   cached marker is valid so long as the same function is called repeatedly.

   This function should be called with care. If this triggers an uncaught
   exception, that exception will bubble up through the caller unless the caller
   has installed a jump. This function may also cause vm->vm_regs to be
   realloc'd, thus invalidating any cached copy of vm->vm_regs.

   The result of this function is the register that the function's value
   returned to, or NULL if there was no returned value. */
lily_value *lily_foreign_call(lily_vm_state *vm, int *cached,
         lily_type *expect_type, lily_value *call_val, int num_values, ...)
{
    lily_function_val *target = call_val->value.function;
    lily_call_frame *calling_frame = vm->call_chain;
    int is_native_target = (target->foreign_func == NULL);
    int have_return = expect_type != NULL;
    int target_need;
    int register_need;
    /* Don't set this just yet: grow_vm_registers may move it. */
    lily_value **vm_regs;
    lily_value *return_reg;

    if (is_native_target)
        target_need = target->reg_count;
    else
        target_need = num_values;

    register_need = vm->num_registers + target_need + have_return;

    if (vm->num_registers + register_need > vm->offset_max_registers)
        grow_vm_registers(vm, register_need);

    /* The vm doesn't increase vm->vm_regs for foreign calls, so that foreign
       calls can use indexes from the caller without copying over. Since this is
       going to put the vm in another function, that increase has to be done.
       This local copy of vm_regs will have [0] set to target the caller's one
       spare register. */
    vm_regs = vm->vm_regs + calling_frame->prev->regs_used;

    if (have_return) {
        return_reg = vm_regs[0];

        if ((return_reg->flags & VAL_IS_NOT_DEREFABLE) == 0)
            lily_deref(return_reg);

        /* The foreign call may have a return that isn't solved, so the caller
           is expected to provide a solved one. */
        return_reg->type = expect_type;
        return_reg->flags = VAL_IS_NIL;
    }
    else
        return_reg = NULL;

    if (*cached == 0) {
        lily_call_frame *caller_frame = vm->call_chain;
        caller_frame->code = vm->foreign_code;
        caller_frame->code_pos = 0;
        caller_frame->return_target = vm_regs[0];
        caller_frame->build_value = NULL;
        caller_frame->line_num = 0;

        if (caller_frame->next == NULL) {
            add_call_frame(vm);
            /* The vm's call chain automatically advances when add_call_frame is
               used. That's useful for the vm, but not here. Rewind the frame
               back so that every invocation of this call will have the same
               call_chain. */
            vm->call_chain = caller_frame;
        }

        lily_call_frame *target_frame = caller_frame->next;
        target_frame->code = target->code;
        target_frame->code_pos = 0;
        target_frame->regs_used = target_need;
        target_frame->function = target;
        target_frame->line_num = 0;
        target_frame->build_value = NULL;
    }

    /* This makes it so the values that get read in will drop into the registers
       that the target will use. */
    vm_regs++;

    /* Read the values in that the caller passed. It is assumed that the caller
       passed a valid number of values. Nothing needs to be done for optional
       arguments: It is up to the target to handle that situation. */
    va_list values;
    va_start(values, num_values);
    int i;
    for (i = 0;i < num_values;i++) {
        lily_value *v = va_arg(values, lily_value *);
        if ((vm_regs[i]->flags & VAL_IS_NOT_DEREFABLE) == 0)
            lily_deref(vm_regs[i]);
        if ((v->flags & VAL_IS_NOT_DEREFABLE) == 0)
            v->value.generic->refcount++;

        *vm_regs[i] = *v;
    }
    va_end(values);

    if (*cached == 0 && is_native_target && i != target->reg_count)
        fix_register_types(vm, target, i);

    vm->vm_regs = vm_regs;
    vm->call_chain = vm->call_chain->next;
    vm->num_registers += target_need;
    *cached = 1;

    if (target->code) {
        /* Do this so the vm knows this function was entered. It'll be offset by
           the vm running into o_return_{val,noval} within the target func. */
        vm->call_depth++;
        lily_vm_execute(vm);
    }
    else {
        /* The drop is so vm_regs[0] targets the return value. */
        vm->vm_regs--;
        target->foreign_func(vm, num_values + 1, foreign_call_stack);
        /* The values set above need to be manually scaled back because foreign
           functions assume the vm will do it for them. */
        vm->call_chain = vm->call_chain->prev;
        vm->num_registers -= target_need;
    }

    /* Don't do "vm->vm_regs = vm_regs", because the target may have caused the
       registers to have reallocated. */
    vm->vm_regs -= vm->call_chain->prev->regs_used;

    return return_reg;
}

/* This ensures that the vm's vm_list (temporary value storage) will have at
   least 'need' extra slots available. */
void lily_vm_list_ensure(lily_vm_state *vm, uint32_t need)
{
    lily_vm_list *vm_list = vm->vm_list;
    if ((vm_list->pos + need) > vm_list->size) {
        while ((vm_list->pos + need) > vm_list->size)
            vm_list->size *= 2;

        vm_list->values = lily_realloc(vm_list->values,
                sizeof(lily_value *) * vm_list->size);
    }
}

/* This is used by a module to SET an exception based on a seed. The import
   context is inferred from the import context of the last function called.
   The exception will have 'msg' set as the exception message.

   While this establishes an error, it does not raise it. The reason is that the
   message may rely on a value that needs to be free'd before the actual raise.
   Making this a two-step process allows an error to be established but for an
   exception to be raised too. */
void lily_vm_set_error(lily_vm_state *vm, lily_base_seed *error_seed,
        const char *msg)
{
    lily_import_entry *seed_import = vm->call_chain->function->import;
    lily_class *raise_class = lily_maybe_dynaload_class(vm->parser, seed_import,
            error_seed->name);

    /* An exception that has been dynaloaded will never have generics, and thus
       will always have a default type. It is thus safe to grab the type of the
       newly-made class. */
    lily_raiser_set_error(vm->raiser, raise_class->type, msg);
}

/* This is like lily_vm_set_error, except that the exception is triggered
   immediately instead of being deferred. */
void lily_vm_module_raise(lily_vm_state *vm, lily_base_seed *error_seed,
        const char *msg)
{
    lily_import_entry *seed_import = vm->call_chain->function->import;
    lily_class *raise_class = lily_maybe_dynaload_class(vm->parser, seed_import,
            error_seed->name);

    lily_raise_type_and_msg(vm->raiser, raise_class->type, msg);
}

/* This raises an exception stored by lily_vm_set_error. */
void lily_vm_raise_prepared(lily_vm_state *vm)
{
    lily_raise_prepared(vm->raiser);
}

/* This calculates a siphash for a given hash value. The siphash is based off of
   the vm's sipkey. The caller is expected to only call this for keys that are
   hashable. */
uint64_t lily_siphash(lily_vm_state *vm, lily_value *key)
{
    int key_cls_id = key->type->cls->id;
    uint64_t key_hash;

    if (key_cls_id == SYM_CLASS_STRING)
        key_hash = siphash24(key->value.string->string,
                key->value.string->size, vm->sipkey);
    else if (key_cls_id == SYM_CLASS_INTEGER)
        key_hash = key->value.integer;
    else if (key_cls_id == SYM_CLASS_DOUBLE)
        /* siphash thinks it's sent a pointer (and will try to deref it), so
           send the address. */
        key_hash = siphash24(&(key->value.doubleval), sizeof(double), vm->sipkey);
    else /* Should not happen, because no other classes are valid keys. */
        key_hash = 0;

    return key_hash;
}

/***
 *      ____
 *     |  _ \ _ __ ___ _ __
 *     | |_) | '__/ _ \ '_ \
 *     |  __/| | |  __/ |_) |
 *     |_|   |_|  \___| .__/
 *                    |_|
 */

/** These functions are concerned with preparing lily_vm_execute to be called
    after reading a script in. Lily stores both defined functions and literal
    values in a giant array so that they can be accessed by index later.
    However, to save memory, it holds them as a linked list during parsing so
    that it doesn't aggressively over or under allocate array space. Now that
    parsing is done, the linked list is mapped over to the array.

    During non-tagged execute, this should happen only once. In tagged mode, it
    happens for each closing ?> tag. **/

static void load_ties_into_readonly(lily_tie **readonly, lily_tie *tie,
        int stop)
{
    while (tie && tie->reg_spot >= stop) {
        readonly[tie->reg_spot] = tie;
        tie = tie->next;
    }
}

/* This loads the symtab's literals and functions into the giant
   vm->readonly_table so that lily_vm_execute can find them later. */
static void setup_readonly_table(lily_vm_state *vm)
{
    lily_symtab *symtab = vm->symtab;

    if (vm->readonly_count == symtab->next_readonly_spot)
        return;

    int count = symtab->next_readonly_spot;
    lily_tie **new_table = lily_realloc(vm->readonly_table,
            count * sizeof(lily_tie *));

    int load_stop = vm->readonly_count;

    load_ties_into_readonly(new_table, symtab->literals, load_stop);
    load_ties_into_readonly(new_table, symtab->function_ties, load_stop);

    vm->readonly_count = symtab->next_readonly_spot;
    vm->readonly_table = new_table;
}

/* Foreign ties are created when a module wants to associate some bit of data
   with a particular register. This happens mostly when dynaloading vars (such
   as sys::argv, stdio, etc.)

   These ties are freed after they are loaded because they are loaded only once
   and are few in number. */
static void load_foreign_ties(lily_vm_state *vm)
{
    lily_tie *tie_iter = vm->symtab->foreign_ties;
    lily_tie *tie_next;
    lily_value **regs_from_main = vm->regs_from_main;

    while (tie_iter) {
        /* Don't use lily_assign_value, because that wants to give the tied
           value a ref. That's bad because then it will have two refs (and the
           tie is just for shifting a value over). */
        lily_value *reg_value = regs_from_main[tie_iter->reg_spot];

        reg_value->type = tie_iter->type;
        /* The flags and type have already been set by vm prep. */
        reg_value->value = tie_iter->value;
        reg_value->flags = (tie_iter->type->cls->flags & VAL_IS_PRIMITIVE);

        tie_next = tie_iter->next;
        lily_free(tie_iter);
        tie_iter = tie_next;
    }

    vm->symtab->foreign_ties = NULL;
}

/* This must be called before lily_vm_execute if the parser has read any data
   in. This makes sure that __main__ has enough register slots, that the
   vm->readonly_table is set, and that foreign ties are loaded. */
void lily_vm_prep(lily_vm_state *vm, lily_symtab *symtab)
{
    lily_function_val *main_function = symtab->main_function;
    int i;

    /* This has to be set before grow_vm_registers because that initializes the
       registers to integer's type. */
    vm->integer_type = symtab->integer_class->type;

    if (main_function->reg_count > vm->offset_max_registers) {
        grow_vm_registers(vm, main_function->reg_count);
        /* grow_vm_registers will move vm->vm_regs (which is supposed to be
           local). That works everywhere...but here. Fix vm_regs back to the
           start because we're still in __main__. */
        vm->vm_regs = vm->regs_from_main;
    }
    else if (vm->vm_regs == NULL) {
        /* This forces vm->vm_regs and vm->regs_from_main to not be NULL. This
           prevents a segfault that happens when __main__ calls something that
           does not have a register need (ex: __import__) and tries to calculate
           the return register. Without this, vm->vm_regs will be NULL and not
           get resized, resulting in a crash. */
        grow_vm_registers(vm, 1);
        vm->vm_regs = vm->regs_from_main;
    }

    lily_value **regs_from_main = vm->regs_from_main;

    for (i = vm->prep_id_start;i < main_function->reg_count;i++) {
        lily_value *reg = regs_from_main[i];
        lily_register_info seed = main_function->reg_info[i];

        /* This allows opcodes to copy over a register value without checking
           if VAL_IS_NIL is set. This is fine, because the value won't actually
           be used if VAL_IS_NIL is set (optimization!) */
        reg->flags = VAL_IS_NIL;
        reg->type = seed.type;
    }

    /* Zap only the slots that new globals need next time. */
    vm->prep_id_start = i;

    /* Symtab is guaranteed to always have a non-NULL tie because the sys
       package creates a tie. */
    if (vm->symtab->foreign_ties)
        load_foreign_ties(vm);

    if (vm->readonly_count != symtab->next_readonly_spot)
        setup_readonly_table(vm);

    vm->num_registers = main_function->reg_count;

    lily_call_frame *first_frame = vm->call_chain;
    first_frame->function = main_function;
    first_frame->code = main_function->code;
    first_frame->regs_used = main_function->reg_count;
    first_frame->return_target = NULL;
    first_frame->code_pos = 0;
    first_frame->build_value = NULL;
    vm->call_depth = 1;
}

/***
 *      _____                     _
 *     | ____|_  _____  ___ _   _| |_ ___
 *     |  _| \ \/ / _ \/ __| | | | __/ _ \
 *     | |___ >  <  __/ (__| |_| | ||  __/
 *     |_____/_/\_\___|\___|\__,_|\__\___|
 *
 */

void lily_vm_execute(lily_vm_state *vm)
{
    uint16_t *code;
    lily_value **regs_from_main;
    lily_value **vm_regs;
    int i, num_registers, offset_max_registers;
    lily_type *cast_type;
    register int64_t for_temp;
    /* This unfortunately has to be volatile because otherwise calltrace() and
       traceback tend to be 'off'. */
    register volatile int code_pos;
    register lily_value *lhs_reg, *rhs_reg, *loop_reg, *step_reg;
    register lily_tie *readonly_val;
    /* These next two are used so that INTDBL operations have a fast way to
       check the type of a register. */
    lily_type *integer_type = vm->symtab->integer_class->type;
    lily_type *double_type = vm->symtab->double_class->type;
    lily_function_val *fval;
    lily_value **upvalues = NULL;

    lily_call_frame *current_frame = vm->call_chain;
    code = current_frame->function->code;

    /* Initialize local vars from the vm state's vars. */
    vm_regs = vm->vm_regs;
    regs_from_main = vm->regs_from_main;
    offset_max_registers = vm->offset_max_registers;
    code_pos = 0;

    lily_jump_link *link = lily_jump_setup(vm->raiser);
    if (setjmp(link->jump) != 0) {
        /* If the current function is a native one, then fix the line
           number of it. Otherwise, leave the line number alone. */
        if (current_frame->function->code != NULL)
            current_frame->line_num = current_frame->code[code_pos+1];

        if (maybe_catch_exception(vm) == 0)
            /* Couldn't catch it. Jump back into parser, which will jump
               back to the caller to give them the bad news. */
            lily_jump_back(vm->raiser);
        else {
            /* The exception was caught, so resync local data. */
            current_frame = vm->call_chain;
            code = current_frame->code;
            code_pos = current_frame->code_pos;
            upvalues = current_frame->upvalues;
            regs_from_main = vm->regs_from_main;
            vm_regs = vm->vm_regs;
            vm->num_registers = (vm_regs - regs_from_main) + current_frame->regs_used;
        }
    }

    num_registers = vm->num_registers;

    while (1) {
        switch(code[code_pos]) {
            case o_fast_assign:
                rhs_reg = vm_regs[code[code_pos+2]];
                lhs_reg = vm_regs[code[code_pos+3]];
                lhs_reg->flags = rhs_reg->flags;
                lhs_reg->value = rhs_reg->value;
                code_pos += 4;
                break;
            case o_get_readonly:
                readonly_val = vm->readonly_table[code[code_pos+2]];
                lhs_reg = vm_regs[code[code_pos+3]];

                lily_deref(lhs_reg);

                lhs_reg->value = readonly_val->value;
                lhs_reg->flags = VAL_IS_LITERAL;
                code_pos += 4;
                break;
            case o_integer_add:
                INTEGER_OP(+)
                break;
            case o_integer_minus:
                INTEGER_OP(-)
                break;
            case o_double_add:
                INTDBL_OP(+)
                break;
            case o_double_minus:
                INTDBL_OP(-)
                break;
            case o_less:
                COMPARE_OP(<, == -1)
                break;
            case o_less_eq:
                COMPARE_OP(<=, <= 0)
                break;
            case o_is_equal:
                EQUALITY_COMPARE_OP(==, == 0)
                break;
            case o_greater:
                COMPARE_OP(>, == 1)
                break;
            case o_greater_eq:
                COMPARE_OP(>, >= 0)
                break;
            case o_not_eq:
                EQUALITY_COMPARE_OP(!=, != 0)
                break;
            case o_jump:
                code_pos = code[code_pos+1];
                break;
            case o_integer_mul:
                INTEGER_OP(*)
                break;
            case o_double_mul:
                INTDBL_OP(*)
                break;
            case o_integer_div:
                /* Before doing INTEGER_OP, check for a division by zero. This
                   will involve some redundant checking of the rhs, but better
                   than dumping INTEGER_OP's contents here or rewriting
                   INTEGER_OP for the special case of division. */
                rhs_reg = vm_regs[code[code_pos+3]];
                if (rhs_reg->value.integer == 0)
                    lily_raise(vm->raiser, lily_DivisionByZeroError,
                            "Attempt to divide by zero.\n");
                INTEGER_OP(/)
                break;
            case o_modulo:
                /* x % 0 will do the same thing as x / 0... */
                rhs_reg = vm_regs[code[code_pos+3]];
                if (rhs_reg->value.integer == 0)
                    lily_raise(vm->raiser, lily_DivisionByZeroError,
                            "Attempt to divide by zero.\n");
                INTEGER_OP(%)
                break;
            case o_left_shift:
                INTEGER_OP(<<)
                break;
            case o_right_shift:
                INTEGER_OP(>>)
                break;
            case o_bitwise_and:
                INTEGER_OP(&)
                break;
            case o_bitwise_or:
                INTEGER_OP(|)
                break;
            case o_bitwise_xor:
                INTEGER_OP(^)
                break;
            case o_double_div:
                /* This is a little more tricky, because the rhs could be a
                   number or an integer... */
                rhs_reg = vm_regs[code[code_pos+3]];
                if (rhs_reg->type->cls->id == SYM_CLASS_INTEGER &&
                    rhs_reg->value.integer == 0)
                    lily_raise(vm->raiser, lily_DivisionByZeroError,
                            "Attempt to divide by zero.\n");
                else if (rhs_reg->type->cls->id == SYM_CLASS_DOUBLE &&
                         rhs_reg->value.doubleval == 0)
                    lily_raise(vm->raiser, lily_DivisionByZeroError,
                            "Attempt to divide by zero.\n");

                INTDBL_OP(/)
                break;
            case o_jump_if:
                lhs_reg = vm_regs[code[code_pos+2]];
                {
                    int cls_id = lhs_reg->type->cls->id;
                    int result;

                    if (cls_id == SYM_CLASS_INTEGER ||
                        cls_id == SYM_CLASS_BOOLEAN)
                        result = (lhs_reg->value.integer == 0);
                    else if (cls_id == SYM_CLASS_DOUBLE)
                        result = (lhs_reg->value.doubleval == 0);
                    else if (cls_id == SYM_CLASS_STRING)
                        result = (lhs_reg->value.string->size == 0);
                    else if (cls_id == SYM_CLASS_LIST)
                        result = (lhs_reg->value.list->num_values == 0);
                    else
                        result = 1;

                    if (result != code[code_pos+1])
                        code_pos = code[code_pos+3];
                    else
                        code_pos += 4;
                }
                break;
            case o_function_call:
            {
                if (vm->call_depth > 100)
                    lily_raise(vm->raiser, lily_RuntimeError,
                            "Function call recursion limit reached.\n");

                if (current_frame->next == NULL)
                    add_call_frame(vm);

                if (code[code_pos+2] == 1)
                    fval = vm->readonly_table[code[code_pos+3]]->value.function;
                else
                    fval = vm_regs[code[code_pos+3]]->value.function;

                int j = code[code_pos+4];
                current_frame->line_num = code[code_pos+1];
                current_frame->code_pos = code_pos + j + 6;
                current_frame->upvalues = upvalues;

                if (fval->code != NULL) {
                    int register_need = fval->reg_count + num_registers;

                    if (register_need > offset_max_registers) {
                        grow_vm_registers(vm, register_need);
                        /* Don't forget to update local info... */
                        regs_from_main       = vm->regs_from_main;
                        vm_regs              = vm->vm_regs;
                        offset_max_registers = vm->offset_max_registers;
                    }

                    /* Prepare the registers for what the function wants.
                       Afterward, update num_registers since prep_registers
                       changes it. */
                    prep_registers(vm, fval, code+code_pos);
                    num_registers = vm->num_registers;

                    current_frame->return_target = vm_regs[code[code_pos+5]];
                    vm_regs = vm_regs + current_frame->regs_used;
                    vm->vm_regs = vm_regs;

                    /* !PAST HERE TARGETS THE NEW FRAME! */

                    current_frame = current_frame->next;
                    vm->call_chain = current_frame;

                    current_frame->function = fval;
                    current_frame->regs_used = fval->reg_count;
                    current_frame->code = fval->code;
                    current_frame->upvalues = NULL;
                    vm->call_depth++;
                    code = fval->code;
                    code_pos = 0;
                    upvalues = NULL;
                }
                else {
                    lily_foreign_func func = fval->foreign_func;

                    current_frame = current_frame->next;
                    vm->call_chain = current_frame;

                    current_frame->function = fval;
                    current_frame->line_num = -1;
                    current_frame->code = NULL;
                    current_frame->build_value = NULL;
                    current_frame->upvalues = NULL;
                    current_frame->regs_used = 1;
                    /* An offset from main does not have to be included, because
                       foreign functions don't have code which can catch an
                       exception. */
                    vm->call_depth++;

                    /* This is done so that the foreign call API can safely
                       store an intermediate value without worrying about having
                       to toggle this. There's no harm in this because there are
                       always two spare registers alloted. */
                    vm->num_registers++;

                    func(vm, j, code+code_pos+5);
                    /* This function may have called the vm, thus growing the
                       number of registers. Copy over important data if that's
                       happened. */
                    if (vm->offset_max_registers != offset_max_registers) {
                        regs_from_main       = vm->regs_from_main;
                        vm_regs              = vm->vm_regs;
                        offset_max_registers = vm->offset_max_registers;
                    }

                    vm->num_registers--;
                    current_frame = current_frame->prev;
                    vm->call_chain = current_frame;

                    code_pos += 6 + j;
                    vm->call_depth--;
                }
            }
                break;
            case o_unary_not:
                lhs_reg = vm_regs[code[code_pos+2]];

                rhs_reg = vm_regs[code[code_pos+3]];
                rhs_reg->flags = VAL_IS_PRIMITIVE;
                rhs_reg->value.integer = !(lhs_reg->value.integer);
                code_pos += 4;
                break;
            case o_unary_minus:
                lhs_reg = vm_regs[code[code_pos+2]];

                rhs_reg = vm_regs[code[code_pos+3]];
                rhs_reg->flags = VAL_IS_PRIMITIVE;
                rhs_reg->value.integer = -(lhs_reg->value.integer);
                code_pos += 4;
                break;
            case o_return_val:
                lhs_reg = current_frame->prev->return_target;
                rhs_reg = vm_regs[code[code_pos+2]];
                lily_assign_value(lhs_reg, rhs_reg);

                /* DO NOT BREAK HERE.
                   These two do the same thing from here on, so fall through to
                   share code. */
            case o_return_noval:
                current_frame->build_value = NULL;

                current_frame = current_frame->prev;
                vm->call_chain = current_frame;
                vm->call_depth--;

                /* The registers that the function last entered are not in use
                   anymore, so they don't count now. */
                num_registers -= current_frame->next->regs_used;
                vm->num_registers = num_registers;
                /* vm_regs adjusts by the count of the now-current function so
                   that vm_regs[0] is the 0 of the caller. */
                vm_regs = vm_regs - current_frame->regs_used;
                vm->vm_regs = vm_regs;
                upvalues = current_frame->upvalues;
                code = current_frame->code;
                code_pos = current_frame->code_pos;
                break;
            case o_get_global:
                rhs_reg = regs_from_main[code[code_pos+2]];
                lhs_reg = vm_regs[code[code_pos+3]];

                lily_assign_value(lhs_reg, rhs_reg);
                code_pos += 4;
                break;
            case o_set_global:
                rhs_reg = vm_regs[code[code_pos+2]];
                lhs_reg = regs_from_main[code[code_pos+3]];

                lily_assign_value(lhs_reg, rhs_reg);
                code_pos += 4;
                break;
            case o_assign:
                rhs_reg = vm_regs[code[code_pos+2]];
                lhs_reg = vm_regs[code[code_pos+3]];

                lily_assign_value(lhs_reg, rhs_reg);
                code_pos += 4;
                break;
            case o_box_assign:
                rhs_reg = vm_regs[code[code_pos+2]];
                lhs_reg = vm_regs[code[code_pos+3]];

                do_o_box_assign(vm, lhs_reg, rhs_reg);
                code_pos += 4;
                break;
            case o_get_item:
                do_o_get_item(vm, code, code_pos);
                code_pos += 5;
                break;
            case o_get_property:
                do_o_get_property(vm, code, code_pos);
                code_pos += 5;
                break;
            case o_set_item:
                do_o_set_item(vm, code, code_pos);
                code_pos += 5;
                break;
            case o_set_property:
                do_o_set_property(vm, code, code_pos);
                code_pos += 5;
                break;
            case o_build_hash:
                do_o_build_hash(vm, code, code_pos);
                code_pos += code[code_pos+2] + 4;
                break;
            case o_build_list_tuple:
                do_o_build_list_tuple(vm, code+code_pos);
                code_pos += code[code_pos+2] + 4;
                break;
            case o_build_enum:
                do_o_build_enum(vm, code+code_pos);
                code_pos += code[code_pos+3] + 5;
                break;
            case o_any_typecast:
                lhs_reg = vm_regs[code[code_pos+3]];
                cast_type = lhs_reg->type;

                rhs_reg = vm_regs[code[code_pos+2]];
                rhs_reg = rhs_reg->value.any->inner_value;

                /* Symtab ensures that two types don't define the same
                   thing, so this is okay. */
                if (cast_type == rhs_reg->type)
                    lily_assign_value(lhs_reg, rhs_reg);
                else {
                    current_frame->line_num = current_frame->code[code_pos+1];

                    lily_raise(vm->raiser, lily_BadTypecastError,
                            "Cannot cast any containing type '^T' to type '^T'.\n",
                            rhs_reg->type, lhs_reg->type);
                }

                code_pos += 4;
                break;
            case o_create_function:
                do_o_create_function(vm, code + code_pos);
                code_pos += 4;
                break;
            case o_set_upvalue:
                lhs_reg = upvalues[code[code_pos + 2]];
                rhs_reg = vm_regs[code[code_pos + 3]];
                if (lhs_reg == NULL)
                    upvalues[code[code_pos + 2]] = make_cell_from(rhs_reg);
                else
                    lily_assign_value(lhs_reg, rhs_reg);

                code_pos += 4;
                break;
            case o_get_upvalue:
                lhs_reg = vm_regs[code[code_pos + 3]];
                rhs_reg = upvalues[code[code_pos + 2]];
                lily_assign_value(lhs_reg, rhs_reg);
                code_pos += 4;
                break;
            case o_setup_optargs:
                do_o_setup_optargs(vm, code, code_pos);
                code_pos += 2 + code[code_pos + 1];
                break;
            case o_integer_for:
                /* loop_reg is an internal counter, while lhs_reg is an external
                   counter. rhs_reg is the stopping point. */
                loop_reg = vm_regs[code[code_pos+2]];
                lhs_reg  = vm_regs[code[code_pos+3]];
                rhs_reg  = vm_regs[code[code_pos+4]];
                step_reg = vm_regs[code[code_pos+5]];

                /* Note the use of the loop_reg. This makes it use the internal
                   counter, and thus prevent user assignments from damaging the loop. */
                for_temp = loop_reg->value.integer + step_reg->value.integer;

                /* This idea comes from seeing Lua do something similar. */
                if ((step_reg->value.integer > 0)
                        /* Positive bound check */
                        ? (for_temp <= rhs_reg->value.integer)
                        /* Negative bound check */
                        : (for_temp >= rhs_reg->value.integer)) {

                    /* Haven't reached the end yet, so bump the internal and
                       external values.*/
                    lhs_reg->value.integer = for_temp;
                    loop_reg->value.integer = for_temp;
                    code_pos += 7;
                }
                else
                    code_pos = code[code_pos+6];

                break;
            case o_push_try:
            {
                if (vm->catch_chain->next == NULL)
                    add_catch_entry(vm);

                lily_vm_catch_entry *catch_entry = vm->catch_chain;
                catch_entry->call_frame = current_frame;
                catch_entry->call_frame_depth = vm->call_depth;
                catch_entry->code_pos = code_pos + 2;
                catch_entry->jump_entry = vm->raiser->all_jumps;
                catch_entry->offset_from_main = (int64_t)(vm_regs - regs_from_main);
                catch_entry->vm_list_pos = vm->vm_list->pos;

                vm->catch_top = vm->catch_chain;
                vm->catch_chain = vm->catch_chain->next;
                code_pos += 3;
                break;
            }
            case o_pop_try:
                vm->catch_chain = vm->catch_top;
                vm->catch_top = vm->catch_top->prev;

                code_pos++;
                break;
            case o_raise:
                lhs_reg = vm_regs[code[code_pos+2]];
                do_o_raise(vm, lhs_reg);
                code_pos += 3;
                break;
            case o_new_instance:
            {
                do_o_new_instance(vm, code+code_pos);
                code_pos += 3;
                break;
            }
            case o_match_dispatch:
            {
                lhs_reg = vm_regs[code[code_pos+2]];
                int variant_id = lhs_reg->value.instance->variant_id;
                lily_class *enum_cls = lhs_reg->type->cls;
                for (i = 0;i < code[code_pos+3];i++) {
                    if (enum_cls->variant_members[i]->variant_id == variant_id)
                        break;
                }

                /* The emitter ensures that the match pattern is exhaustive, so
                   something must have been found. */
                code_pos = code[code_pos + 4 + i];
                break;
            }
            case o_variant_decompose:
            {
                rhs_reg = vm_regs[code[code_pos + 2]];
                lily_value **decompose_values = rhs_reg->value.instance->values;

                /* Each variant value gets mapped away to a register. The
                   emitter ensures that the decomposition won't go too far. */
                for (i = 0;i < code[code_pos+3];i++) {
                    lhs_reg = vm_regs[code[code_pos + 4 + i]];
                    lily_assign_value(lhs_reg, decompose_values[i]);
                }

                code_pos += 4 + i;
                break;
            }
            case o_create_closure:
                upvalues = do_o_create_closure(vm, code+code_pos);
                code_pos += 4;
                break;
            case o_load_class_closure:
                upvalues = do_o_load_class_closure(vm, code, code_pos);
                code_pos += 5;
                break;
            case o_load_closure:
                upvalues = do_o_load_closure(vm, code+code_pos);
                code_pos = code[code_pos+2] + 4;
                break;
            case o_for_setup:
                loop_reg = vm_regs[code[code_pos+2]];
                /* lhs_reg is the start, rhs_reg is the stop. */
                step_reg = vm_regs[code[code_pos+5]];
                lhs_reg = vm_regs[code[code_pos+3]];
                rhs_reg = vm_regs[code[code_pos+4]];

                if (step_reg->value.integer == 0)
                    lily_raise(vm->raiser, lily_ValueError,
                               "for loop step cannot be 0.\n");

                /* Do a negative step to offset falling into o_for_loop. */
                loop_reg->value.integer =
                        lhs_reg->value.integer - step_reg->value.integer;
                loop_reg->flags = VAL_IS_PRIMITIVE;

                code_pos += 7;
                break;
            case o_return_from_vm:
                lily_release_jump(vm->raiser);
                return;
        }
    }
}
