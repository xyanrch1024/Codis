import tkinter as tk
from tkinter import messagebox
import random
import copy

class SudokuGame:
    def __init__(self, master):
        self.master = master
        self.master.title("数独游戏")
        self.master.geometry("600x700")
        self.master.configure(bg='#f0f0f0')
        
        # 游戏状态
        self.board = [[0 for _ in range(9)] for _ in range(9)]
        self.solution = [[0 for _ in range(9)] for _ in range(9)]
        self.original_board = [[0 for _ in range(9)] for _ in range(9)]
        self.selected_cell = None
        self.cells = []
        
        # 创建UI
        self.create_widgets()
        self.new_game()
        
    def create_widgets(self):
        # 标题
        title_label = tk.Label(self.master, text="数独游戏", 
                              font=("Arial", 24, "bold"), bg='#f0f0f0')
        title_label.pack(pady=10)
        
        # 游戏板框架
        self.game_frame = tk.Frame(self.master, bg='black', bd=3, relief=tk.RAISED)
        self.game_frame.pack(pady=10)
        
        # 创建9x9的网格
        for i in range(9):
            row = []
            for j in range(9):
                # 计算边框宽度
                border_top = 3 if i % 3 == 0 else 1
                border_left = 3 if j % 3 == 0 else 1
                border_bottom = 3 if i == 8 else 1
                border_right = 3 if j == 8 else 1
                
                cell_frame = tk.Frame(self.game_frame, bg='black',
                                    highlightthickness=0)
                cell_frame.grid(row=i, column=j, padx=(border_left, border_right),
                              pady=(border_top, border_bottom))
                
                cell = tk.Label(cell_frame, text="", width=3, height=2,
                              font=("Arial", 18, "bold"), bg='white',
                              relief=tk.FLAT, cursor="hand2")
                cell.pack()
                cell.bind("<Button-1>", lambda e, row=i, col=j: self.cell_clicked(row, col))
                
                row.append(cell)
            self.cells.append(row)
        
        # 控制按钮框架
        control_frame = tk.Frame(self.master, bg='#f0f0f0')
        control_frame.pack(pady=10)
        
        # 新游戏按钮
        new_game_btn = tk.Button(control_frame, text="新游戏", 
                               command=self.new_game,
                               font=("Arial", 12), bg='#4CAF50', fg='white',
                               padx=20, pady=10, relief=tk.RAISED)
        new_game_btn.pack(side=tk.LEFT, padx=5)
        
        # 检查答案按钮
        check_btn = tk.Button(control_frame, text="检查答案", 
                            command=self.check_solution,
                            font=("Arial", 12), bg='#2196F3', fg='white',
                            padx=20, pady=10, relief=tk.RAISED)
        check_btn.pack(side=tk.LEFT, padx=5)
        
        # 提示按钮
        hint_btn = tk.Button(control_frame, text="提示", 
                           command=self.give_hint,
                           font=("Arial", 12), bg='#FF9800', fg='white',
                           padx=20, pady=10, relief=tk.RAISED)
        hint_btn.pack(side=tk.LEFT, padx=5)
        
        # 数字按钮框架
        number_frame = tk.Frame(self.master, bg='#f0f0f0')
        number_frame.pack(pady=10)
        
        # 数字按钮1-9
        for num in range(1, 10):
            btn = tk.Button(number_frame, text=str(num), 
                          command=lambda n=num: self.number_clicked(n),
                          font=("Arial", 14, "bold"), bg='#9C27B0', fg='white',
                          width=4, height=2, relief=tk.RAISED)
            btn.grid(row=0, column=num-1, padx=2, pady=2)
        
        # 清除按钮
        clear_btn = tk.Button(number_frame, text="清除", 
                            command=self.clear_cell,
                            font=("Arial", 12), bg='#f44336', fg='white',
                            padx=15, pady=10, relief=tk.RAISED)
        clear_btn.grid(row=1, column=4, padx=2, pady=2)
        
        # 绑定键盘事件
        self.master.bind("<Key>", self.key_pressed)
        
    def generate_sudoku(self):
        # 生成完整的数独解决方案
        board = [[0 for _ in range(9)] for _ in range(9)]
        self.fill_board(board)
        self.solution = copy.deepcopy(board)
        
        # 移除一些数字创建谜题
        cells_to_remove = random.randint(40, 55)
        for _ in range(cells_to_remove):
            row = random.randint(0, 8)
            col = random.randint(0, 8)
            board[row][col] = 0
        
        self.board = copy.deepcopy(board)
        self.original_board = copy.deepcopy(board)
        
    def fill_board(self, board):
        # 使用回溯算法填充数独板
        empty = self.find_empty(board)
        if not empty:
            return True
        
        row, col = empty
        numbers = list(range(1, 10))
        random.shuffle(numbers)
        
        for num in numbers:
            if self.is_valid(board, num, row, col):
                board[row][col] = num
                
                if self.fill_board(board):
                    return True
                
                board[row][col] = 0
        
        return False
    
    def find_empty(self, board):
        for i in range(9):
            for j in range(9):
                if board[i][j] == 0:
                    return (i, j)
        return None
    
    def is_valid(self, board, num, row, col):
        # 检查行
        for j in range(9):
            if board[row][j] == num:
                return False
        
        # 检查列
        for i in range(9):
            if board[i][col] == num:
                return False
        
        # 检查3x3宫格
        box_row = (row // 3) * 3
        box_col = (col // 3) * 3
        for i in range(box_row, box_row + 3):
            for j in range(box_col, box_col + 3):
                if board[i][j] == num:
                    return False
        
        return True
    
    def new_game(self):
        self.generate_sudoku()
        self.update_display()
        self.selected_cell = None
        
    def update_display(self):
        for i in range(9):
            for j in range(9):
                value = self.board[i][j]
                cell = self.cells[i][j]
                
                if value == 0:
                    cell.config(text="", bg='white')
                else:
                    if self.original_board[i][j] != 0:
                        cell.config(text=str(value), bg='#e0e0e0', fg='black')
                    else:
                        cell.config(text=str(value), bg='white', fg='blue')
                
                # 高亮选中的单元格
                if self.selected_cell == (i, j):
                    cell.config(bg='#ffeb3b')
                elif self.selected_cell and self.is_same_group(self.selected_cell, (i, j)):
                    cell.config(bg='#fff9c4')
    
    def is_same_group(self, cell1, cell2):
        row1, col1 = cell1
        row2, col2 = cell2
        
        # 同一行
        if row1 == row2:
            return True
        # 同一列
        if col1 == col2:
            return True
        # 同一宫格
        if (row1 // 3) == (row2 // 3) and (col1 // 3) == (col2 // 3):
            return True
        
        return False
    
    def cell_clicked(self, row, col):
        self.selected_cell = (row, col)
        self.update_display()
    
    def number_clicked(self, num):
        if self.selected_cell:
            row, col = self.selected_cell
            if self.original_board[row][col] == 0:  # 只能修改非原始数字
                self.board[row][col] = num
                self.update_display()
    
    def clear_cell(self):
        if self.selected_cell:
            row, col = self.selected_cell
            if self.original_board[row][col] == 0:  # 只能清除非原始数字
                self.board[row][col] = 0
                self.update_display()
    
    def key_pressed(self, event):
        if self.selected_cell:
            row, col = self.selected_cell
            if self.original_board[row][col] == 0:  # 只能修改非原始数字
                if event.char.isdigit() and event.char != '0':
                    self.board[row][col] = int(event.char)
                    self.update_display()
                elif event.keysym in ['BackSpace', 'Delete', 'Clear']:
                    self.board[row][col] = 0
                    self.update_display()
    
    def check_solution(self):
        # 检查当前解答是否正确
        for i in range(9):
            for j in range(9):
                if self.board[i][j] == 0:
                    messagebox.showwarning("未完成", "数独还没有完成！")
                    return
        
        # 检查是否与解决方案匹配
        for i in range(9):
            for j in range(9):
                if self.board[i][j] != self.solution[i][j]:
                    messagebox.showerror("错误", "答案有误，请继续努力！")
                    return
        
        messagebox.showinfo("恭喜", "恭喜你！数独解答正确！")
    
    def give_hint(self):
        # 找到一个空单元格并填入正确答案
        empty_cells = []
        for i in range(9):
            for j in range(9):
                if self.board[i][j] == 0:
                    empty_cells.append((i, j))
        
        if empty_cells:
            row, col = random.choice(empty_cells)
            self.board[row][col] = self.solution[row][col]
            self.selected_cell = (row, col)
            self.update_display()
        else:
            messagebox.showinfo("提示", "没有空单元格需要提示！")

def main():
    root = tk.Tk()
    game = SudokuGame(root)
    root.mainloop()

if __name__ == "__main__":
    main()