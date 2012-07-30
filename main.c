#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* configuration for shape/obj growth pattern */
#define SHAPE_INITIAL_SLOTS 4
#define SHAPE_GROW_SLOTS 4

/* TODO: chunked growth strategy for objects -- these
    arent used yet, but will eliminate a bunch of alloc
    churn when they are */
#define OBJ_INITIAL_SLOTS 4
#define OBJ_GROW_SLOTS 4

/* NOTE: for use with a GC. this code generates some garbage,
    and doesn't make any attempt to clean up after itself. */

/* tagged-ptr/smallint support. */

typedef struct { ssize_t val; } val_t;

static inline val_t val_from_int(int x) { val_t v = { .val = x << 1 }; return v; }
static inline val_t val_from_ptr(void * p) { val_t v = { .val = (ssize_t)p | 1 }; return v; }
static int val_is_ptr(val_t v) { return v.val & 1; }
static int val_to_int(val_t v) { return v.val >> 1; }
static void * val_to_ptr(val_t v) { return (void *)(v.val & ~1); }

struct member {
    char const * name;
    val_t ref;
};

struct shape {
    struct shape * redir;
    int num_data_items;
    int num_items;
    int capacity;
    struct member members[0];
};

struct obj {
    struct shape * shape;
    struct obj * redir;
    val_t members[0];
};

static struct shape * root_shape;

struct obj * obj_alloc(void) {
    struct obj * o = malloc(sizeof(struct obj));
    o->shape = root_shape;
    o->redir = 0;
    return o;
}

struct shape * shape_alloc(void) {
    struct shape * s = malloc(sizeof(struct shape) + SHAPE_INITIAL_SLOTS * sizeof(struct member));
    s->redir = 0;
    s->num_items = 0;
    s->num_data_items = 0;
    s->capacity = SHAPE_INITIAL_SLOTS;
    return s;
}

int shape_find_member(struct shape * s, char const * member) {
    for (int i=0; i<s->num_items; i++)
        if (!strcmp(s->members[i].name, member))
            return i;

    return s->num_items;    /* it's going to go here */
}

struct shape * shape_grow(struct shape * s) {
    size_t old_size = sizeof(struct shape) + s->capacity * sizeof(struct member);
    size_t new_size = old_size + sizeof(struct member) * SHAPE_GROW_SLOTS;

    struct shape * ss = malloc(new_size);
    memcpy(ss, s, old_size);
    ss->capacity = s->capacity + SHAPE_GROW_SLOTS;
    s->redir = ss;
    ss->redir = 0;

    printf( "shape_grow %p -> %p (%zd->%zd bytes)\n",
        s, ss, old_size, new_size );
    return ss;
}

struct shape * shape_add_transition(struct shape * s, char const * member) {
    if (s->num_items == s->capacity)
        s = shape_grow(s);

    size_t old_size = sizeof(struct shape) + 
        s->num_items * sizeof(struct member);
    size_t new_size = old_size + sizeof(struct member);

    struct shape * ns = malloc(new_size);

    memcpy(ns, s, old_size);

    ns->num_data_items = s->num_data_items + 1;
    ns->num_items = s->num_items + 1;

    s->members[s->num_items].name = member;
    s->members[s->num_items].ref = val_from_ptr(ns);

    ns->members[s->num_items].name = member;
    ns->members[s->num_items].ref = val_from_int(
        offsetof(struct obj, members[s->num_data_items]));

    s->num_items++;

    ns->redir = 0;

    printf( "shape_add_transition %p -> %p for member %s\n",
        s, ns, member );

    return ns;
}

struct obj * obj_change_shape(struct obj * o, struct shape * ns) {
    size_t old_size = sizeof(struct obj) 
        + sizeof(val_t) * o->shape->num_data_items;
    size_t new_size = old_size + sizeof(val_t);

    printf( "obj_change_shape old_size=%zd new_size=%zd\n",
        old_size, new_size );

    struct obj * no = malloc(new_size);
    memcpy(no, o, old_size);

    o->redir = no;
    no->redir = 0;

    no->shape = ns;

    printf( "obj_change_shape %p -> %p (shape %p -> %p)\n",
        o, no, o->shape, no->shape );

    return no;
}

static inline struct obj * canonicalize(struct obj * o) {
    struct obj * co = o;
    while (co->redir) co = co->redir;
    if (o->redir) o->redir = co;

    while (co->shape->redir) co->shape = co->shape->redir;
    return co;
}

struct obj * obj_set_member(struct obj * o, char const * member, val_t value) {
    o = canonicalize(o);
    struct shape * s = o->shape;

    int member_index = shape_find_member(s, member);

    if (member_index == s->num_items) {
        /* doesn't exist; neither does a transition yet */
        struct shape * ns = shape_add_transition(s, member);
        o = obj_change_shape(o, ns);
        s = o->shape;
    }
    else if (val_is_ptr(s->members[member_index].ref)) {
        /* existing transition; follow it and change o's shape */
        o = obj_change_shape(o, val_to_ptr(s->members[member_index].ref));
        s = o->shape;
    }

    /* finally, actually set the member */
    int offset = val_to_int(s->members[member_index].ref);
    val_t * v = (val_t *)((size_t)(o) + offset);

    *v = value;
    return o;
}

val_t obj_get_member(struct obj * o, char const * member) {
    o = canonicalize(o);
    struct shape * s = o->shape;

    int member_index = shape_find_member(s, member);
    if (member_index == s->num_items || val_is_ptr(s->members[member_index].ref)) {
        /* TODO: exception model */
        printf( "!! no member %s in obj %p\n", member, o );
        return val_from_ptr(0);
    }

    int offset = val_to_int(s->members[member_index].ref);
    val_t * v = (val_t *)((size_t)(o) + offset);
    return *v;
}

void dump_shape(struct shape * s) {
    printf( "----dump shape %p:\n", s );

    while( s->redir )
        printf( "  - redir %p\n", s = s->redir );

    printf( "  - %d slots: (%d actual, %d cap)\n", s->num_items, s->num_data_items, s->capacity );
    for( int i = 0; i < s->num_items; i++ ) {
        if (val_is_ptr(s->members[i].ref))
            printf( "  %d) %s -> shape %p\n", i,
                s->members[i].name,
                val_to_ptr(s->members[i].ref) );
        else
            printf( "  %d) %s -> offset %d\n", i,
                s->members[i].name,
                val_to_int(s->members[i].ref) );
    }

    printf( "----end of dump %p\n", s );
}

void dump_val(char const * p, val_t v) {
    if (val_is_ptr(v))
        printf( "val `%s`: ptr %p\n", p, val_to_ptr(v) );
    else
        printf( "val `%s`: int %d\n", p, val_to_int(v) );
}

int main(void) {
    root_shape = shape_alloc();

    printf( "sizes: obj: %zd shape: %zd member: %zd\n",
        sizeof(struct obj), sizeof(struct shape), sizeof(struct member) );

    dump_shape(root_shape);

    shape_add_transition( root_shape, "y" );

    dump_shape(root_shape);

    struct obj * o = obj_alloc();

    printf( "alloc %p\n", o );
    
    o = obj_set_member(o, "x", val_from_int(42));
    o = obj_set_member(o, "x", val_from_int(12));

    dump_shape(root_shape);

    struct obj * o2 = obj_alloc();
    o2 = obj_set_member(o2, "y", val_from_int(-17));
    o2 = obj_set_member(o2, "x", val_from_int(42));

    dump_shape(canonicalize(o)->shape);
    dump_shape(canonicalize(o2)->shape);

    val_t v = obj_get_member(o2, "x");
    val_t v2 = obj_get_member(o2, "y");
    val_t v3 = obj_get_member(o2, "z");   /* doesnt exist */

    dump_val("x", v);
    dump_val("y", v2);
    dump_val("z", v3);

    return 0;
}
