#include <iostream>
#include <vector>
#include <random>
#include <ctime>
#include <algorithm>
#include <iomanip>
#include <limits>

using namespace std;

class SudokuGame {
private:
    vector<vector<int>> board;
    vector<vector<int>> solution;
    vector<vector<int>> original_board;
    int selected_row;
    int selected_col;
    
public:
    SudokuGame() : selected_row(-1), selected_col(-1) {
        board = vector<vector<int>>(9, vector<int>(9, 0));
        solution = vector<vector<int>>(9, vector<int>(9, 0));
        original_board = vector<vector<int>>(9, vector<int>(9, 0));
    }
    
    void generate_sudoku() {
        // 生成完整的数独解决方案
        fill_board(solution);
        
        // 复制解决方案到游戏板
        board = solution;
        original_board = solution;
        
        // 移除一些数字创建谜题
        int cells_to_remove = 40 + rand() % 16; // 40-55个空格
        for (int i = 0; i < cells_to_remove; i++) {
            int row = rand() % 9;
            int col = rand() % 9;
            board[row][col] = 0;
            original_board[row][col] = 0;
        }
    }
    
    bool fill_board(vector<vector<int>>& board) {
        for (int row = 0; row < 9; row++) {
            for (int col = 0; col < 9; col++) {
                if (board[row][col] == 0) {
                    vector<int> numbers = {1, 2, 3, 4, 5, 6, 7, 8, 9};
                    random_shuffle(numbers.begin(), numbers.end());
                    
                    for (int num : numbers) {
                        if (is_valid(board, num, row, col)) {
                            board[row][col] = num;
                            
                            if (fill_board(board)) {
                                return true;
                            }
                            
                            board[row][col] = 0;
                        }
                    }
                    return false;
                }
            }
        }
        return true;
    }
    
    bool is_valid(const vector<vector<int>>& board, int num, int row, int col) {
        // 检查行
        for (int j = 0; j < 9; j++) {
            if (board[row][j] == num) {
                return false;
            }
        }
        
        // 检查列
        for (int i = 0; i < 9; i++) {
            if (board[i][col] == num) {
                return false;
            }
        }
        
        // 检查3x3宫格
        int box_row = (row / 3) * 3;
        int box_col = (col / 3) * 3;
        for (int i = box_row; i < box_row + 3; i++) {
            for (int j = box_col; j < box_col + 3; j++) {
                if (board[i][j] == num) {
                    return false;
                }
            }
        }
        
        return true;
    }
    
    void display_board() {
        cout << "\n   ";
        for (int i = 0; i < 9; i++) {
            cout << " " << i + 1 << "  ";
        }
        cout << "\n   +---+---+---+---+---+---+---+---+---+\n";
        
        for (int i = 0; i < 9; i++) {
            cout << i + 1 << " |";
            for (int j = 0; j < 9; j++) {
                if (get_selected_row() == i && get_selected_col() == j) {
                    cout << "[" << setw(2) << (board[i][j] ? to_string(board[i][j]) : " ") << "]";
                } else if (original_board[i][j] != 0) {
                    cout << " " << setw(2) << board[i][j] << " ";
                } else {
                    cout << " " << setw(2) << (board[i][j] ? to_string(board[i][j]) : " ") << " ";
                }
                
                if (j % 3 == 2 && j < 8) {
                    cout << "|";
                }
            }
            cout << "|\n";
            
            if (i % 3 == 2 && i < 8) {
                cout << "   +---+---+---+---+---+---+---+---+---+\n";
            }
        }
        cout << "   +---+---+---+---+---+---+---+---+---+\n";
    }
    
    void select_cell(int row, int col) {
        if (row >= 0 && row < 9 && col >= 0 && col < 9) {
            set_selected_row(row);
            set_selected_col(col);
        }
    }
    
    int get_selected_row() const { return selected_row; }
    int get_selected_col() const { return selected_col; }
    void set_selected_row(int row) { selected_row = row; }
    void set_selected_col(int col) { selected_col = col; }
    
    bool place_number(int num) {
        if (selected_row == -1 || selected_col == -1) {
            cout << "请先选择一个单元格！\n";
            return false;
        }
        
        if (original_board[selected_row][selected_col] != 0) {
            cout << "这个单元格是固定的，不能修改！\n";
            return false;
        }
        
        if (num < 1 || num > 9) {
            cout << "数字必须在1-9之间！\n";
            return false;
        }
        
        board[selected_row][selected_col] = num;
        return true;
    }
    
    void clear_cell() {
        if (selected_row == -1 || selected_col == -1) {
            cout << "请先选择一个单元格！\n";
            return;
        }
        
        if (original_board[selected_row][selected_col] != 0) {
            cout << "这个单元格是固定的，不能清除！\n";
            return;
        }
        
        board[selected_row][selected_col] = 0;
    }
    
    bool is_complete() {
        for (int i = 0; i < 9; i++) {
            for (int j = 0; j < 9; j++) {
                if (board[i][j] == 0) {
                    return false;
                }
            }
        }
        return true;
    }
    
    bool is_correct() {
        for (int i = 0; i < 9; i++) {
            for (int j = 0; j < 9; j++) {
                if (board[i][j] != solution[i][j]) {
                    return false;
                }
            }
        }
        return true;
    }
    
    void give_hint() {
        vector<pair<int, int>> empty_cells;
        for (int i = 0; i < 9; i++) {
            for (int j = 0; j < 9; j++) {
                if (board[i][j] == 0) {
                    empty_cells.push_back({i, j});
                }
            }
        }
        
        if (empty_cells.empty()) {
            cout << "没有空单元格需要提示！\n";
            return;
        }
        
        int index = rand() % empty_cells.size();
        int row = empty_cells[index].first;
        int col = empty_cells[index].second;
        
        board[row][col] = solution[row][col];
        selected_row = row;
        selected_col = col;
        
        cout << "提示：单元格 (" << row + 1 << "," << col + 1 << ") 的答案是 " << solution[row][col] << "\n";
    }
    
    void new_game() {
        generate_sudoku();
        selected_row = -1;
        selected_col = -1;
        cout << "新游戏开始！\n";
    }
    
    void show_instructions() {
        cout << "\n=== 数独游戏说明 ===\n";
        cout << "1. 输入行号和列号选择单元格 (1-9)\n";
        cout << "2. 输入数字1-9在选中单元格放置数字\n";
        cout << "3. 输入0清除选中单元格\n";
        cout << "4. 输入'c'清除当前单元格\n";
        cout << "5. 输入'h'获取提示\n";
        cout << "6. 输入'check'检查答案\n";
        cout << "7. 输入'new'开始新游戏\n";
        cout << "8. 输入'quit'退出游戏\n";
        cout << "==================\n";
    }
};

int main() {
    srand(time(0));
    
    SudokuGame game;
    game.new_game();
    game.show_instructions();
    
    while (true) {
        game.display_board();
        
        cout << "\n当前选择：";
        if (game.selected_row != -1 && game.selected_col != -1) {
            cout << "(" << game.selected_row + 1 << "," << game.selected_col + 1 << ")";
        } else {
            cout << "无";
        }
        cout << "\n请输入命令：";
        
        string input;
        cin >> input;
        
        if (input == "quit") {
            cout << "游戏结束，再见！\n";
            break;
        }
        else if (input == "new") {
            game.new_game();
            continue;
        }
        else if (input == "check") {
            if (game.is_complete()) {
                if (game.is_correct()) {
                    cout << "恭喜！数独解答正确！\n";
                } else {
                    cout << "答案有误，请继续努力！\n";
                }
            } else {
                cout << "数独还没有完成！\n";
            }
            continue;
        }
        else if (input == "h") {
            game.give_hint();
            continue;
        }
        else if (input == "c") {
            game.clear_cell();
            continue;
        }
        else if (input == "help") {
            game.show_instructions();
            continue;
        }
        
        // 尝试解析坐标或数字
        try {
            if (input.length() == 1 && input[0] >= '1' && input[0] <= '9') {
                // 选择行
                int row = stoi(input) - 1;
                cout << "请输入列号 (1-9)：";
                string col_input;
                cin >> col_input;
                int col = stoi(col_input) - 1;
                game.select_cell(row, col);
            }
            else if (input.length() == 2 && input[0] >= '1' && input[0] <= '9' && 
                     input[1] >= '1' && input[1] <= '9') {
                // 选择单元格
                int row = stoi(input.substr(0, 1)) - 1;
                int col = stoi(input.substr(1, 2)) - 1;
                game.select_cell(row, col);
            }
            else if (input.length() == 3 && input[0] >= '1' && input[0] <= '9' && 
                     input[1] >= '1' && input[1] <= '9' && input[2] >= '1' && input[2] <= '9') {
                // 选择单元格并放置数字
                int row = stoi(input.substr(0, 1)) - 1;
                int col = stoi(input.substr(1, 2)) - 1;
                int num = stoi(input.substr(2, 3));
                game.select_cell(row, col);
                game.place_number(num);
            }
            else {
                cout << "无效的输入！请输入有效的坐标或命令。\n";
            }
        } catch (...) {
            cout << "输入格式错误！请重新输入。\n";
            cin.clear();
            cin.ignore(numeric_limits<streamsize>::max(), '\n');
        }
    }
    
    return 0;
}