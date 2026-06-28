#!/bin/bash

# 文件统计脚本
# 功能：统计当前目录下的文件数量、行数、文件类型等信息

echo "=== 文件统计报告 ==="
echo "统计时间: $(date)"
echo "工作目录: $(pwd)"
echo

# 1. 基本统计
total_files=$(find . -type f | wc -l)
total_dirs=$(find . -type d | wc -l)
echo "1. 基本统计:"
echo "   文件总数: $total_files"
echo "   目录总数: $total_dirs"
echo

# 2. 按文件扩展名统计
echo "2. 按文件类型统计:"
find . -type f | sed 's/.*\.//' | sort | uniq -c | sort -nr | while read count ext; do
    if [ -n "$ext" ]; then
        printf "   %-10s: %d 个文件\n" ".$ext" "$count"
    fi
done
echo

# 3. 行数统计
echo "3. 代码行数统计:"
total_lines=0
c_files=$(find . -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" | wc -l)
if [ $c_files -gt 0 ]; then
    c_lines=$(find . -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" -exec wc -l {} + | tail -1 | awk '{print $1}')
    echo "   C/C++ 文件: $c_files 个, 总行数: $c_lines"
    total_lines=$((total_lines + c_lines))
fi

py_files=$(find . -name "*.py" | wc -l)
if [ $py_files -gt 0 ]; then
    py_lines=$(find . -name "*.py" -exec wc -l {} + | tail -1 | awk '{print $1}')
    echo "   Python 文件: $py_files 个, 总行数: $py_lines"
    total_lines=$((total_lines + py_lines))
fi

sh_files=$(find . -name "*.sh" | wc -l)
if [ $sh_files -gt 0 ]; then
    sh_lines=$(find . -name "*.sh" -exec wc -l {} + | tail -1 | awk '{print $1}')
    echo "   Shell 脚本: $sh_files 个, 总行数: $sh_lines"
    total_lines=$((total_lines + sh_lines))
fi

other_lines=$(find . -type f ! \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" -o -name "*.py" -o -name "*.sh" \) -exec wc -l {} + 2>/dev/null | tail -1 | awk '{print $1}' 2>/dev/null || echo "0")
echo "   其他文件: $((total_files - c_files - py_files - sh_files)) 个, 总行数: $other_lines"
echo "   总计行数: $((total_lines + other_lines))"
echo

# 4. 文件大小统计
echo "4. 文件大小统计:"
total_size=$(du -sh . | cut -f1)
echo "   总大小: $total_size"

# 统计不同大小范围的文件
echo "   文件大小分布:"
small_files=$(find . -type f -size -c | wc -l)
medium_files=$(find . -type f -size +c -size -1M | wc -l)
large_files=$(find . -type f -size +1M | wc -l)
echo "     < 1KB: $small_files 个文件"
echo "     1KB-1MB: $medium_files 个文件"
echo "     > 1MB: $large_files 个文件"
echo

# 5. 最大和最小的文件
echo "5. 文件大小排行:"
echo "   最大的5个文件:"
find . -type f -exec ls -lh {} + | sort -k5 -hr | head -6 | awk '{printf "   %-50s %s\n", $9, $5}'

echo
echo "   最小的5个文件:"
find . -type f -exec ls -lh {} + | sort -k5 -h | head -6 | awk '{printf "   %-50s %s\n", $9, $5}'
echo

# 6. 最近修改的文件
echo "6. 最近修改的文件 (前10个):"
find . -type f -printf "%T@ %p\n" | sort -n | tail -10 | cut -d' ' -f2- | while read file; do
    mod_time=$(stat -c "%Y" "$file" 2>/dev/null | xargs -I {} date -d "@{}" "+%Y-%m-%d %H:%M:%S" 2>/dev/null || echo "未知时间")
    echo "   $file (修改时间: $mod_time)"
done
echo

echo "=== 统计完成 ==="