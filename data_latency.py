def read_data(filename):
    with open(filename, 'r') as file:
        lines = file.readlines()

    # 将每一行中的数字提取出来，并存储到列表中
    numbers = []
    for line in lines:
        # 假设数字之间由空格分隔
        numbers.extend([float(num) for num in line.split()])
    return numbers

def find_50latency(numbers):
    # 计算中位数
    # numbers.sort()
    n = len(numbers)
    if n == 0:
        return None  # 如果列表为空，则返回None
    else:
        return numbers[n // 2]

def find_99latency(numbers):
    # 计算中位数
    # numbers.sort()
    n = len(numbers)
    if n == 0:
        return None  # 如果列表为空，则返回None
    else:
        return numbers[int(n * 0.99)]

def find_999latency(numbers):
    # 计算中位数
    # numbers.sort()
    n = len(numbers)
    if n == 0:
        return None  # 如果列表为空，则返回None
    else:
        return numbers[int(n * 0.999)]

# 假设文件名为 'numbers.txt'
filename = 'build/sorted_output.txt'
numbers = read_data(filename)
p50 = find_50latency(numbers)
if p50 is not None:
    print(f"The p50 latency is: {p50}")

p99 = find_99latency(numbers)
if p99 is not None:
    print(f"The p99 latency is: {p99}")

p999 = find_999latency(numbers)
if p999 is not None:
    print(f"The p999 latency is: {p999}")