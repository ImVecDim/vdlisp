#include <stdio.h>
#include <stdlib.h>

// 这个小程序不是解释器的一部分，而是单纯模拟高频分配场景做对照测试。
typedef struct Node {
    void *data;
    struct Node *next;
} Node;

typedef struct List {
    Node *head;
} List;

List *create_list() {
    // 每次都新建一个空表头，故意不做回收来放大压力。
    List *list = (List *)malloc(sizeof(List));
    list->head = NULL;
    return list;
}

void cons(List *list, void *data) {
    // 头插法构造单链表，足够模拟大量 pair/cons 风格分配。
    Node *new_node = (Node *)malloc(sizeof(Node));
    new_node->data = data;
    new_node->next = list->head;
    list->head = new_node;
}

typedef int (*Func)(int);

Func create_func() {
    // 这里只是人为制造一批“函数对象”风格的分配。
    Func f = (Func)malloc(sizeof(Func));
    return f;
}

int main() {
    // 三重循环把分配数量放大到足以触发内存池/分配器边界行为。
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
            // 故意不 free，用于模拟“生命周期集中在进程末尾”的极端场景。
        }
    }
    printf("pool_test_ok\n");
    return 0;
}