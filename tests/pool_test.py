# Python 版本的轻量压力样例，用来快速模拟链表与闭包对象的重复创建。
for i in range(100):
    for j in range(100):
        tmp = []
        for k in range(100):
            tmp = [[k]] + tmp
        fns = []
        for k in range(100):
            fns = [lambda x: x + 1] + fns
print("pool_test_ok")