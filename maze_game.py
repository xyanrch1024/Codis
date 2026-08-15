import random
import time
import os

class Item:
    def __init__(self, name, description, effect_type=None, effect_value=0):
        self.name = name
        self.description = description
        self.effect_type = effect_type
        self.effect_value = effect_value

class Room:
    def __init__(self, name, description):
        self.name = name
        self.description = description
        self.exits = {}
        self.items = []
        self.monster = None
        self.treasure = False
        self.puzzle_solved = False
        self.puzzle_question = None
        self.puzzle_answer = None

class Monster:
    def __init__(self, name, hp, attack):
        self.name = name
        self.hp = hp
        self.attack = attack

class Player:
    def __init__(self, name):
        self.name = name
        self.hp = 100
        self.max_hp = 100
        self.attack = 10
        self.defense = 5
        self.inventory = []
        self.treasures_found = 0
        
    def take_damage(self, damage):
        actual_damage = max(1, damage - self.defense)
        self.hp = max(0, self.hp - actual_damage)
        return actual_damage
        
    def heal(self, amount):
        self.hp = min(self.max_hp, self.hp + amount)
        
    def add_item(self, item):
        self.inventory.append(item)
        if item.effect_type == "attack":
            self.attack += item.effect_value
        elif item.effect_type == "defense":
            self.defense += item.effect_value
        elif item.effect_type == "heal":
            self.heal(item.effect_value)

class MazeGame:
    def __init__(self):
        self.player = None
        self.current_room = None
        self.rooms = {}
        self.game_over = False
        self.setup_game()
        
    def setup_game(self):
        # 创建房间
        entrance = Room("入口大厅", "你站在一个古老的迷宫入口，四周墙壁上刻着神秘的符文。")
        entrance.exits = {"east": "神秘图书馆", "north": "战斗大厅"}
        entrance.items = [Item("小刀", "一把锋利的小刀，可以用来战斗。", "attack", 3)]
        
        library = Room("神秘图书馆", "这里堆满了古老的书籍，空气中弥漫着墨水的味道。")
        library.exits = {"west": "入口大厅", "north": "宝藏室"}
        library.items = [Item("魔法书", "一本古老的魔法书，可以增加你的攻击力。", "attack", 5)]
        
        battle_room = Room("战斗大厅", "这个大厅中央有一只凶猛的狼人！它正盯着你。")
        battle_room.exits = {"south": "入口大厅", "east": "花园"}
        battle_room.monster = Monster("狼人", 30, 8)
        
        treasure_room = Room("宝藏室", "这个房间闪闪发光，但被一道魔法门锁住了。你需要解决谜题才能进入。")
        treasure_room.exits = {"south": "神秘图书馆"}
        treasure_room.treasure = True
        treasure_room.puzzle_question = "什么东西越洗越脏？"
        treasure_room.puzzle_answer = "2"
        treasure_room.items = [Item("钥匙", "一把神秘的钥匙，可以打开某些门。")]
        
        garden = Room("花园", "一个美丽的花园，充满了奇异的植物和花朵。")
        garden.exits = {"west": "战斗大厅", "east": "地下密室"}
        garden.items = [Item("草药", "可以恢复少量生命值的草药。", "heal", 20)]
        
        dungeon = Room("地下密室", "阴暗潮湿的密室，你听到了奇怪的声音。")
        dungeon.exits = {"west": "花园", "north": "最终宝藏室"}
        dungeon.monster = Monster("骷髅战士", 25, 6)
        
        final_treasure = Room("最终宝藏室", "这是迷宫的最终宝藏室！巨大的宝藏就在眼前！")
        final_treasure.exits = {"south": "地下密室"}
        final_treasure.treasure = True
        
        self.rooms = {
            "入口大厅": entrance,
            "神秘图书馆": library,
            "战斗大厅": battle_room,
            "宝藏室": treasure_room,
            "花园": garden,
            "地下密室": dungeon,
            "最终宝藏室": final_treasure
        }
        
        self.current_room = entrance
        
    def start_game(self):
        print("欢迎来到神秘迷宫寻宝游戏！")
        print("=" * 50)
        print("你将扮演一个勇敢的冒险者，探索神秘的迷宫并寻找宝藏！")
        self.player = Player("冒险者")
        print(f"你好，勇敢的 {self.player.name}！")
        print("你的目标是探索迷宫，收集宝藏，并安全地逃离。")
        print(f"你拥有 {self.player.hp} 点生命值。")
        print("=" * 50)
        
        self.game_loop()
        
    def clear_screen(self):
        os.system('clear' if os.name == 'posix' else 'cls')
        
    def show_room_info(self):
        room = self.current_room
        print(f"\n=== {room.name} ===")
        print(room.description)
        
        if room.exits:
            print(f"出口方向: {', '.join(room.exits.keys())}")
            
        if room.items:
            print("房间内的物品:")
            for item in room.items:
                print(f"- {item.name}: {item.description}")
                
        if room.monster:
            print(f"危险！你发现了 {room.monster.name}！")
            print(f"它拥有 {room.monster.hp} 点生命值，攻击力为 {room.monster.attack}。")
            
        if room.treasure and not room.puzzle_solved:
            print("这里有一道谜题需要解决才能获得宝藏。")
            
        if room.treasure and room.puzzle_solved:
            print("恭喜！你发现了宝藏！")
            
    def show_player_status(self):
        print(f"\n=== {self.player.name} 的状态 ===")
        print(f"生命值: {self.player.hp}/{self.player.max_hp}")
        print(f"攻击力: {self.player.attack}")
        print(f"防御力: {self.player.defense}")
        print(f"收集的宝藏: {self.player.treasures_found}")
        
        if self.player.inventory:
            print("背包中的物品:")
            for item in self.player.inventory:
                print(f"- {item.name}")
                
    def move_player(self, direction):
        if direction in self.current_room.exits:
            new_room_name = self.current_room.exits[direction]
            self.current_room = self.rooms[new_room_name]
            print(f"你移动到了 {self.current_room.name}。")
        else:
            print("那个方向没有出口！")
            
    def pick_item(self):
        room = self.current_room
        if not room.items:
            print("这个房间里没有物品可以拾取。")
            return
            
        print("你想拾取哪个物品？")
        for i, item in enumerate(room.items):
            print(f"{i + 1}. {item.name}")
            
        try:
            choice = int(input("请输入选择: ")) - 1
            if 0 <= choice < len(room.items):
                item = room.items.pop(choice)
                self.player.add_item(item)
                print(f"你拾取了 {item.name}。")
                
                if item.effect_type == "attack":
                    print(f"你的攻击力增加了 {item.effect_value} 点！")
                elif item.effect_type == "defense":
                    print(f"你的防御力增加了 {item.effect_value} 点！")
                elif item.effect_type == "heal":
                    print(f"你恢复了 {item.effect_value} 点生命值！")
            else:
                print("无效的选择。")
        except ValueError:
            print("请输入有效的数字。")
            
    def combat(self):
        room = self.current_room
        if not room.monster:
            print("这里没有怪物可以战斗。")
            return
            
        monster = room.monster
        print(f"战斗开始！你与 {monster.name} 展开了战斗！")
        
        while self.player.hp > 0 and monster.hp > 0:
            # 玩家攻击
            player_damage = random.randint(1, self.player.attack)
            monster.hp -= player_damage
            print(f"你对 {monster.name} 造成了 {player_damage} 点伤害！")
            
            if monster.hp <= 0:
                print(f"你击败了 {monster.name}！")
                room.monster = None
                break
                
            # 怪物攻击
            monster_damage = monster.attack - self.player.defense
            monster_damage = max(1, monster_damage)
            actual_damage = self.player.take_damage(monster_damage)
            print(f"{monster.name} 对你造成了 {actual_damage} 点伤害！")
            
            if self.player.hp <= 0:
                print(f"你被 {monster.name} 击败了...")
                self.game_over = True
                return
                
            time.sleep(1)
            
    def solve_puzzle(self):
        room = self.current_room
        if not room.treasure or room.puzzle_solved:
            print("这里没有谜题需要解决。")
            return
            
        print("你发现了一个神秘的谜题...")
        print(f"谜题：{room.puzzle_question}")
        print("1. 水  2. 手  3. 布  4. 答案是2")
        
        answer = input("请输入你的选择 (1-4): ")
        
        if answer == room.puzzle_answer:
            print("恭喜！谜题解决了！你获得了宝藏！")
            room.puzzle_solved = True
            self.player.treasures_found += 1
        else:
            print("答案错误，再想想吧！")
            
    def show_help(self):
        print("\n=== 游戏帮助 ===")
        print("look - 查看当前房间信息")
        print("status - 查看你的状态")
        print("move [方向] - 移动到指定方向 (north, south, east, west)")
        print("take - 拾取物品")
        print("fight - 战斗")
        print("puzzle - 解决谜题")
        print("inventory - 查看背包")
        print("help - 显示帮助")
        print("quit - 退出游戏")
        
    def game_loop(self):
        print("\n游戏开始！输入 'help' 查看可用命令。")
        
        while not self.game_over:
            print("\n> ", end="")
            command = input().strip().lower()
            
            if command == "quit":
                print("感谢游玩神秘迷宫寻宝游戏！")
                break
            elif command == "help":
                self.show_help()
            elif command == "look":
                self.show_room_info()
            elif command == "status":
                self.show_player_status()
            elif command == "inventory":
                self.show_player_status()
            elif command == "take":
                self.pick_item()
            elif command == "fight":
                self.combat()
            elif command == "puzzle":
                self.solve_puzzle()
            elif command.startswith("move "):
                direction = command[5:]
                self.move_player(direction)
            else:
                print("无效的命令。输入 'help' 查看可用命令。")
                
            # 检查游戏结束条件
            if self.player.treasures_found >= 2:
                print("\n恭喜！你收集了足够的宝藏，成功逃离了迷宫！")
                print("你是一个真正的冒险者！")
                break
                
            if self.player.hp <= 0:
                print("\n游戏结束！你被击败了...")
                break

def main():
    game = MazeGame()
    game.start_game()

if __name__ == "__main__":
    main()