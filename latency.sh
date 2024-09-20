#!/bin/bash

# 定义一个包含文件名的数组
# file_list=("build/insert_lat*.txt")

# 定义一个空的数组来存储匹配的文件路径
declare -a file_list=()

# 定义你想要匹配的正则表达式
# 例如，匹配当前目录及子目录下所有以.txt结尾的文件
regex="insert*\.txt" # 使用find命令查找匹配的文件，并使用read命令将结果存储到数组中
while IFS= read -r -d '' file_path; do  file_list+=("$file_path")
done < <(find build/ -type f -name "$regex" -print0)

regex="search*\.txt" # 使用find命令查找匹配的文件，并使用read命令将结果存储到数组中
while IFS= read -r -d '' file_path; do  file_list+=("$file_path")
done < <(find build/ -type f -name "$regex" -print0)

# for file in "${file_list[@]}";
#   do echo "$file"
# done

# 指定输出文件的名称
output_file="build/sorted_output.txt"

# 创建一个空文件作为输出文件
> "$output_file"
# 使用 for 循环逐个读取文件列表中的文件
for file in "${file_list[@]}"; do
  # 检查文件是否存在
  if [ -f "$file" ]; then    # 将文件内容追加到输出文件中    
    cat "$file" >> "$output_file"  
  else    echo "文件 $file 不存在，跳过。"
  fi
done

# 对输出文件进行排序
sort -n "$output_file" -o "$output_file"

python3 data_latency.py

rm build/*lat*