#include <stdio.h>
#include <stdlib.h>

typedef struct Node {
    void *data;
    struct Node *next;
} Node;

typedef struct List {
    Node *head;
} List;

List *create_list() {
    List *list = (List *)malloc(sizeof(List));
    list->head = NULL;
    return list;
}

void cons(List *list, void *data) {
    Node *new_node = (Node *)malloc(sizeof(Node));
    new_node->data = data;
    new_node->next = list->head;
    list->head = new_node;
}

typedef int (*Func)(int);

Func create_func() {
    // Simulate function creation by allocating a dummy function pointer
    Func f = (Func)malloc(sizeof(Func));
    return f;
}

int main() {
    for (int i = 0; i < 100; i++) {
        for (int j = 0; j < 100; j++) {
            List *tmp = create_list();
            for (int k = 0; k < 100; k++) {
                List *inner_list = create_list();
                cons(tmp, inner_list);
            }
            List *fns = create_list();
            for (int k = 0; k < 100; k++) {
                Func f = create_func();
                cons(fns, f);
            }
            // Note: Not freeing memory to simulate pool stress
        }
    }
    printf("pool_test_ok\n");
    return 0;
}