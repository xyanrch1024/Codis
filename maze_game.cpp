#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <random>
#include <ctime>
#include <algorithm>

using namespace std;

// 游戏物品结构
struct Item {
    string name;
    string description;
    bool is_equipped = false;
};

// 房间结构
struct Room {
    string name;
    string description;
    vector<string> exits;
    vector<Item> items;
    bool has_monster = false;
    string monster_name;
    int monster_hp = 0;
    bool has_treasure = false;
    bool puzzle_solved = false;
};

// 玩家结构
struct Player {
    string name;
    int hp = 100;
    int max_hp = 100;
    int attack = 10;
    int defense = 5;
    vector<Item> inventory;
    vector<string> visited_rooms;
    int treasure_count = 0;
};

// 游戏类
class MazeGame {
private:
    vector<Room> rooms;
    Player player;
    random_device rd;
    mt19937 gen;
    bool game_over = false;
    
public:
    MazeGame() : gen(rd()) {
        initialize_rooms();
        initialize_player();
    }
    
    void initialize_rooms() {
        // 创建8个房间
        Room room1;
        room1.name = "入口大厅";
        room1.description = "你站在一个古老的迷宫入口，四周墙壁上刻着神秘的符文。";
        room1.exits = {"east", "north"};
        Item knife = {"小刀", "一把锋利的小刀，可以用来战斗。", false};
        room1.items.push_back(knife);
        
        Room room2;
        room2.name = "神秘图书馆";
        room2.description = "这里堆满了古老的书籍，空气中弥漫着墨水的味道。";
        room2.exits = {"west", "north"};
        Item magic_book = {"魔法书", "一本古老的魔法书，可以增加你的攻击力。", false};
        room2.items.push_back(magic_book);
        
        Room room3;
        room3.name = "战斗大厅";
        room3.description = "这个大厅中央有一只凶猛的狼人！它正盯着你。";
        room3.exits = {"south", "east"};
        room3.has_monster = true;
        room3.monster_name = "狼人";
        room3.monster_hp = 30;
        
        Room room4;
        room4.name = "宝藏室";
        room4.description = "这个房间闪闪发光，但被一道魔法门锁住了。你需要解决谜题才能进入。";
        room4.exits = {"west", "north"};
        room4.has_treasure = true;
        Item key = {"钥匙", "一把神秘的钥匙，可以打开某些门。", false};
        room4.items.push_back(key);
        
        Room room5;
        room5.name = "花园";
        room5.description = "一个美丽的花园，充满了奇异的植物和花朵。";
        room5.exits = {"south", "east"};
        Item herb = {"草药", "可以恢复少量生命值的草药。", false};
        room5.items.push_back(herb);
        
        Room room6;
        room6.name = "地下密室";
        room6.description = "阴暗潮湿的密室，你听到了奇怪的声音。";
        room6.exits = {"north", "west"};
        room6.has_monster = true;
        room6.monster_name = "骷髅战士";
        room6.monster_hp = 25;
        
        Room room7;
        room7.name = "魔法实验室";
        room7.description = "这里充满了各种魔法物品和药剂。";
        room7.exits = {"west", "south"};
        Item armor = {"护甲", "一件坚固的护甲，可以增加你的防御力。", false};
        room7.items.push_back(armor);
        
        Room room8;
        room8.name = "最终宝藏室";
        room8.description = "这是迷宫的最终宝藏室！巨大的宝藏就在眼前！";
        room8.exits = {"north"};
        room8.has_treasure = true;
        room8.puzzle_solved = true;
        
        rooms = {room1, room2, room3, room4, room5, room6, room7, room8};
    }
    
    void initialize_player() {
        cout << "欢迎来到神秘迷宫寻宝游戏！" << endl;
        cout << "请输入你的名字: ";
        getline(cin, player.name);
        cout << "你好，勇敢的冒险者 " << player.name << "！" << endl;
        cout << "你的目标是探索迷宫，收集宝藏，并安全地逃离。" << endl;
        cout << "你拥有 " << player.hp << " 点生命值。" << endl;
    }
    
    void show_room_info(int room_index) {
        Room& room = rooms[room_index];
        cout << "\n=== " << room.name << " ===" << endl;
        cout << room.description << endl;
        
        // 显示出口
        if (!room.exits.empty()) {
            cout << "出口方向: ";
            for (const auto& exit : room.exits) {
                cout << exit << " ";
            }
            cout << endl;
        }
        
        // 显示物品
        if (!room.items.empty()) {
            cout << "房间内的物品:" << endl;
            for (const auto& item : room.items) {
                cout << "- " << item.name << ": " << item.description << endl;
            }
        }
        
        // 显示怪物
        if (room.has_monster) {
            cout << "危险！你发现了 " << room.monster_name << "！" << endl;
            cout << "它拥有 " << room.monster_hp << " 点生命值。" << endl;
        }
        
        // 显示宝藏
        if (room.has_treasure && room.puzzle_solved) {
            cout << "恭喜！你发现了宝藏！" << endl;
            player.treasure_count++;
        }
        
        // 显示谜题
        if (room.has_treasure && !room.puzzle_solved) {
            cout << "这里有一道谜题需要解决才能获得宝藏。" << endl;
        }
    }
    
    void show_player_status() {
        cout << "\n=== " << player.name << " 的状态 ===" << endl;
        cout << "生命值: " << player.hp << "/" << player.max_hp << endl;
        cout << "攻击力: " << player.attack << endl;
        cout << "防御力: " << player.defense << endl;
        cout << "收集的宝藏: " << player.treasure_count << endl;
        
        if (!player.inventory.empty()) {
            cout << "背包中的物品:" << endl;
            for (const auto& item : player.inventory) {
                cout << "- " << item.name;
                if (item.is_equipped) {
                    cout << " (已装备)";
                }
                cout << endl;
            }
        }
    }
    
    void move_player(const string& direction) {
        int current_room_index = find_current_room();
        Room& current_room = rooms[current_room_index];
        
        auto it = find(current_room.exits.begin(), current_room.exits.end(), direction);
        if (it != current_room.exits.end()) {
            // 简单的房间连接逻辑
            int new_room_index = (current_room_index + 1) % rooms.size();
            player.visited_rooms.push_back(rooms[new_room_index].name);
            cout << "你移动到了 " << rooms[new_room_index].name << "。" << endl;
        } else {
            cout << "那个方向没有出口！" << endl;
        }
    }
    
    int find_current_room() {
        if (player.visited_rooms.empty()) {
            return 0;
        }
        string current_room_name = player.visited_rooms.back();
        for (int i = 0; i < rooms.size(); i++) {
            if (rooms[i].name == current_room_name) {
                return i;
            }
        }
        return 0;
    }
    
    void pick_item(int room_index) {
        Room& room = rooms[room_index];
        if (room.items.empty()) {
            cout << "这个房间里没有物品可以拾取。" << endl;
            return;
        }
        
        cout << "你想拾取哪个物品？" << endl;
        for (int i = 0; i < room.items.size(); i++) {
            cout << i + 1 << ". " << room.items[i].name << endl;
        }
        
        int choice;
        cin >> choice;
        cin.ignore();
        
        if (choice >= 1 && choice <= room.items.size()) {
            Item item = room.items[choice - 1];
            player.inventory.push_back(item);
            room.items.erase(room.items.begin() + choice - 1);
            cout << "你拾取了 " << item.name << "。" << endl;
            
            // 应用物品效果
            if (item.name == "魔法书") {
                player.attack += 5;
                cout << "你的攻击力增加了5点！" << endl;
            } else if (item.name == "护甲") {
                player.defense += 3;
                cout << "你的防御力增加了3点！" << endl;
            } else if (item.name == "草药") {
                player.hp = min(player.hp + 20, player.max_hp);
                cout << "你恢复了20点生命值！" << endl;
            }
        } else {
            cout << "无效的选择。" << endl;
        }
    }
    
    void combat(int room_index) {
        Room& room = rooms[room_index];
        if (!room.has_monster) {
            cout << "这里没有怪物可以战斗。" << endl;
            return;
        }
        
        cout << "战斗开始！你与 " << room.monster_name << " 展开了战斗！" << endl;
        
        while (player.hp > 0 && room.monster_hp > 0) {
            // 玩家攻击
            uniform_int_distribution<> player_damage(1, player.attack);
            int damage_to_monster = player_damage(gen);
            room.monster_hp -= damage_to_monster;
            cout << "你对 " << room.monster_name << " 造成了 " << damage_to_monster << " 点伤害！" << endl;
            
            if (room.monster_hp <= 0) {
                cout << "你击败了 " << room.monster_name << "！" << endl;
                room.has_monster = false;
                break;
            }
            
            // 怪物攻击
            uniform_int_distribution<> monster_damage(1, 15);
            int damage_to_player = monster_damage(gen) - player.defense;
            damage_to_player = max(1, damage_to_player); // 最少造成1点伤害
            player.hp -= damage_to_player;
            cout << room.monster_name << " 对你造成了 " << damage_to_player << " 点伤害！" << endl;
            
            if (player.hp <= 0) {
                cout << "你被 " << room.monster_name << " 击败了..." << endl;
                game_over = true;
                return;
            }
        }
    }
    
    void solve_puzzle(int room_index) {
        Room& room = rooms[room_index];
        if (!room.has_treasure || room.puzzle_solved) {
            cout << "这里没有谜题需要解决。" << endl;
            return;
        }
        
        cout << "你发现了一个神秘的谜题..." << endl;
        cout << "谜题：什么东西越洗越脏？" << endl;
        cout << "1. 水  2. 手  3. 布  4. 答案是2" << endl;
        
        int answer;
        cin >> answer;
        cin.ignore();
        
        if (answer == 4) {
            cout << "恭喜！谜题解决了！你获得了宝藏！" << endl;
            room.puzzle_solved = true;
            player.treasure_count++;
        } else {
            cout << "答案错误，再想想吧！" << endl;
        }
    }
    
    void show_help() {
        cout << "\n=== 游戏帮助 ===" << endl;
        cout << "look - 查看当前房间信息" << endl;
        cout << "status - 查看你的状态" << endl;
        cout << "move [方向] - 移动到指定方向 (north, south, east, west)" << endl;
        cout << "take - 拾取物品" << endl;
        cout << "fight - 战斗" << endl;
        cout << "puzzle - 解决谜题" << endl;
        cout << "inventory - 查看背包" << endl;
        cout << "help - 显示帮助" << endl;
        cout << "quit - 退出游戏" << endl;
    }
    
    void run() {
        cout << "\n游戏开始！输入 'help' 查看可用命令。" << endl;
        
        while (!game_over) {
            cout << "\n> ";
            string command;
            getline(cin, command);
            
            // 转换为小写
            transform(command.begin(), command.end(), command.begin(), ::tolower);
            
            if (command == "quit") {
                cout << "感谢游玩神秘迷宫寻宝游戏！" << endl;
                break;
            } else if (command == "help") {
                show_help();
            } else if (command == "look") {
                int current_room_index = find_current_room();
                show_room_info(current_room_index);
            } else if (command == "status") {
                show_player_status();
            } else if (command == "inventory") {
                show_player_status();
            } else if (command == "take") {
                int current_room_index = find_current_room();
                pick_item(current_room_index);
            } else if (command == "fight") {
                int current_room_index = find_current_room();
                combat(current_room_index);
            } else if (command == "puzzle") {
                int current_room_index = find_current_room();
                solve_puzzle(current_room_index);
            } else if (command.substr(0, 5) == "move ") {
                string direction = command.substr(5);
                move_player(direction);
            } else {
                cout << "无效的命令。输入 'help' 查看可用命令。" << endl;
            }
            
            // 检查游戏结束条件
            if (player.treasure_count >= 3) {
                cout << "\n恭喜！你收集了足够的宝藏，成功逃离了迷宫！" << endl;
                cout << "你是一个真正的冒险者！" << endl;
                break;
            }
            
            if (player.hp <= 0) {
                cout << "\n游戏结束！你被击败了..." << endl;
                break;
            }
        }
    }
};

int main() {
    srand(time(0));
    MazeGame game;
    game.run();
    return 0;
}