class Item {
    constructor(name, description, effectType = null, effectValue = 0) {
        this.name = name;
        this.description = description;
        this.effectType = effectType;
        this.effectValue = effectValue;
    }
}

class Room {
    constructor(name, description) {
        this.name = name;
        this.description = description;
        this.exits = {};
        this.items = [];
        this.monster = null;
        this.treasure = false;
        this.puzzleSolved = false;
        this.puzzleQuestion = null;
        this.puzzleAnswer = null;
    }
}

class Monster {
    constructor(name, hp, attack) {
        this.name = name;
        this.hp = hp;
        this.attack = attack;
    }
}

class Player {
    constructor(name) {
        this.name = name;
        this.hp = 100;
        this.maxHp = 100;
        this.attack = 10;
        this.defense = 5;
        this.inventory = [];
        this.treasuresFound = 0;
    }

    takeDamage(damage) {
        const actualDamage = Math.max(1, damage - this.defense);
        this.hp = Math.max(0, this.hp - actualDamage);
        return actualDamage;
    }

    heal(amount) {
        this.hp = Math.min(this.maxHp, this.hp + amount);
    }

    addItem(item) {
        this.inventory.push(item);
        if (item.effectType === "attack") {
            this.attack += item.effectValue;
        } else if (item.effectType === "defense") {
            this.defense += item.effectValue;
        } else if (item.effectType === "heal") {
            this.heal(item.effectValue);
        }
    }
}

class MazeGame {
    constructor() {
        this.player = null;
        this.currentRoom = null;
        this.rooms = {};
        this.gameOver = false;
        this.setupGame();
    }

    setupGame() {
        // 创建房间
        const entrance = new Room("入口大厅", "你站在一个古老的迷宫入口，四周墙壁上刻着神秘的符文。");
        entrance.exits = { east: "神秘图书馆", north: "战斗大厅" };
        entrance.items = [new Item("小刀", "一把锋利的小刀，可以用来战斗。", "attack", 3)];

        const library = new Room("神秘图书馆", "这里堆满了古老的书籍，空气中弥漫着墨水的味道。");
        library.exits = { west: "入口大厅", north: "宝藏室" };
        library.items = [new Item("魔法书", "一本古老的魔法书，可以增加你的攻击力。", "attack", 5)];

        const battleRoom = new Room("战斗大厅", "这个大厅中央有一只凶猛的狼人！它正盯着你。");
        battleRoom.exits = { south: "入口大厅", east: "花园" };
        battleRoom.monster = new Monster("狼人", 30, 8);

        const treasureRoom = new Room("宝藏室", "这个房间闪闪发光，但被一道魔法门锁住了。你需要解决谜题才能进入。");
        treasureRoom.exits = { south: "神秘图书馆" };
        treasureRoom.treasure = true;
        treasureRoom.puzzleQuestion = "什么东西越洗越脏？";
        treasureRoom.puzzleAnswer = "2";
        treasureRoom.items = [new Item("钥匙", "一把神秘的钥匙，可以打开某些门。")];

        const garden = new Room("花园", "一个美丽的花园，充满了奇异的植物和花朵。");
        garden.exits = { west: "战斗大厅", east: "地下密室" };
        garden.items = [new Item("草药", "可以恢复少量生命值的草药。", "heal", 20)];

        const dungeon = new Room("地下密室", "阴暗潮湿的密室，你听到了奇怪的声音。");
        dungeon.exits = { west: "花园", north: "最终宝藏室" };
        dungeon.monster = new Monster("骷髅战士", 25, 6);

        const finalTreasure = new Room("最终宝藏室", "这是迷宫的最终宝藏室！巨大的宝藏就在眼前！");
        finalTreasure.exits = { south: "地下密室" };
        finalTreasure.treasure = true;

        this.rooms = {
            "入口大厅": entrance,
            "神秘图书馆": library,
            "战斗大厅": battleRoom,
            "宝藏室": treasureRoom,
            "花园": garden,
            "地下密室": dungeon,
            "最终宝藏室": finalTreasure
        };

        this.currentRoom = entrance;
    }

    startGame() {
        console.log("欢迎来到神秘迷宫寻宝游戏！");
        console.log("=".repeat(50));
        console.log("你将扮演一个勇敢的冒险者，探索神秘的迷宫并寻找宝藏！");
        
        this.player = new Player("冒险者");
        console.log(`你好，勇敢的 ${this.player.name}！`);
        console.log("你的目标是探索迷宫，收集宝藏，并安全地逃离。");
        console.log(`你拥有 ${this.player.hp} 点生命值。`);
        console.log("=".repeat(50));
        
        this.gameLoop();
    }

    showRoomInfo() {
        const room = this.currentRoom;
        console.log(`\n=== ${room.name} ===`);
        console.log(room.description);
        
        if (Object.keys(room.exits).length > 0) {
            console.log(`出口方向: ${Object.keys(room.exits).join(', ')}`);
        }
        
        if (room.items.length > 0) {
            console.log("房间内的物品:");
            room.items.forEach(item => {
                console.log(`- ${item.name}: ${item.description}`);
            });
        }
        
        if (room.monster) {
            console.log(`危险！你发现了 ${room.monster.name}！`);
            console.log(`它拥有 ${room.monster.hp} 点生命值，攻击力为 ${room.monster.attack}。`);
        }
        
        if (room.treasure && !room.puzzleSolved) {
            console.log("这里有一道谜题需要解决才能获得宝藏。");
        }
        
        if (room.treasure && room.puzzleSolved) {
            console.log("恭喜！你发现了宝藏！");
        }
    }

    showPlayerStatus() {
        console.log(`\n=== ${this.player.name} 的状态 ===`);
        console.log(`生命值: ${this.player.hp}/${this.player.maxHp}`);
        console.log(`攻击力: ${this.player.attack}`);
        console.log(`防御力: ${this.player.defense}`);
        console.log(`收集的宝藏: ${this.player.treasuresFound}`);
        
        if (this.player.inventory.length > 0) {
            console.log("背包中的物品:");
            this.player.inventory.forEach(item => {
                console.log(`- ${item.name}`);
            });
        }
    }

    movePlayer(direction) {
        if (direction in this.currentRoom.exits) {
            const newRoomName = this.currentRoom.exits[direction];
            this.currentRoom = this.rooms[newRoomName];
            console.log(`你移动到了 ${this.currentRoom.name}。`);
        } else {
            console.log("那个方向没有出口！");
        }
    }

    pickItem() {
        const room = this.currentRoom;
        if (room.items.length === 0) {
            console.log("这个房间里没有物品可以拾取。");
            return;
        }
        
        console.log("你想拾取哪个物品？");
        room.items.forEach((item, index) => {
            console.log(`${index + 1}. ${item.name}`);
        });
        
        const choice = parseInt(prompt("请输入选择: ")) - 1;
        if (choice >= 0 && choice < room.items.length) {
            const item = room.items.splice(choice, 1)[0];
            this.player.addItem(item);
            console.log(`你拾取了 ${item.name}。`);
            
            if (item.effectType === "attack") {
                console.log(`你的攻击力增加了 ${item.effectValue} 点！`);
            } else if (item.effectType === "defense") {
                console.log(`你的防御力增加了 ${item.effectValue} 点！`);
            } else if (item.effectType === "heal") {
                console.log(`你恢复了 ${item.effectValue} 点生命值！`);
            }
        } else {
            console.log("无效的选择。");
        }
    }

    combat() {
        const room = this.currentRoom;
        if (!room.monster) {
            console.log("这里没有怪物可以战斗。");
            return;
        }
        
        const monster = room.monster;
        console.log(`战斗开始！你与 ${monster.name} 展开了战斗！`);
        
        while (this.player.hp > 0 && monster.hp > 0) {
            // 玩家攻击
            const playerDamage = Math.floor(Math.random() * this.player.attack) + 1;
            monster.hp -= playerDamage;
            console.log(`你对 ${monster.name} 造成了 ${playerDamage} 点伤害！`);
            
            if (monster.hp <= 0) {
                console.log(`你击败了 ${monster.name}！`);
                room.monster = null;
                break;
            }
            
            // 怪物攻击
            const monsterDamage = Math.max(1, monster.attack - this.player.defense);
            const actualDamage = this.player.takeDamage(monsterDamage);
            console.log(`${monster.name} 对你造成了 ${actualDamage} 点伤害！`);
            
            if (this.player.hp <= 0) {
                console.log(`你被 ${monster.name} 击败了...`);
                this.gameOver = true;
                return;
            }
            
            // 添加延迟让玩家能看到战斗过程
            setTimeout(() => {}, 1000);
        }
    }

    solvePuzzle() {
        const room = this.currentRoom;
        if (!room.treasure || room.puzzleSolved) {
            console.log("这里没有谜题需要解决。");
            return;
        }
        
        console.log("你发现了一个神秘的谜题...");
        console.log(`谜题：${room.puzzleQuestion}`);
        console.log("1. 水  2. 手  3. 布  4. 答案是2");
        
        const answer = prompt("请输入你的选择 (1-4): ");
        
        if (answer === room.puzzleAnswer) {
            console.log("恭喜！谜题解决了！你获得了宝藏！");
            room.puzzleSolved = true;
            this.player.treasuresFound += 1;
        } else {
            console.log("答案错误，再想想吧！");
        }
    }

    showHelp() {
        console.log("\n=== 游戏帮助 ===");
        console.log("look - 查看当前房间信息");
        console.log("status - 查看你的状态");
        console.log("move [方向] - 移动到指定方向 (north, south, east, west)");
        console.log("take - 拾取物品");
        console.log("fight - 战斗");
        console.log("puzzle - 解决谜题");
        console.log("inventory - 查看背包");
        console.log("help - 显示帮助");
        console.log("quit - 退出游戏");
    }

    gameLoop() {
        console.log("\n游戏开始！输入 'help' 查看可用命令。");
        
        while (!this.gameOver) {
            const command = prompt("\n> ").trim().toLowerCase();
            
            if (command === "quit") {
                console.log("感谢游玩神秘迷宫寻宝游戏！");
                break;
            } else if (command === "help") {
                this.showHelp();
            } else if (command === "look") {
                this.showRoomInfo();
            } else if (command === "status") {
                this.showPlayerStatus();
            } else if (command === "inventory") {
                this.showPlayerStatus();
            } else if (command === "take") {
                this.pickItem();
            } else if (command === "fight") {
                this.combat();
            } else if (command === "puzzle") {
                this.solvePuzzle();
            } else if (command.startsWith("move ")) {
                const direction = command.substring(5);
                this.movePlayer(direction);
            } else {
                console.log("无效的命令。输入 'help' 查看可用命令。");
            }
            
            // 检查游戏结束条件
            if (this.player.treasuresFound >= 2) {
                console.log("\n恭喜！你收集了足够的宝藏，成功逃离了迷宫！");
                console.log("你是一个真正的冒险者！");
                break;
            }
            
            if (this.player.hp <= 0) {
                console.log("\n游戏结束！你被击败了...");
                break;
            }
        }
    }
}

// 为了在Node.js环境中运行，我们需要重写prompt函数
function prompt(question) {
    const readline = require('readline');
    const rl = readline.createInterface({
        input: process.stdin,
        output: process.stdout
    });
    
    return new Promise((resolve) => {
        rl.question(question, (answer) => {
            rl.close();
            resolve(answer);
        });
    });
}

// 如果直接运行这个文件，启动游戏
if (require.main === module) {
    // 由于Node.js的异步特性，我们需要调整游戏循环
    const game = new MazeGame();
    
    // 创建一个简单的命令行界面
    const readline = require('readline');
    const rl = readline.createInterface({
        input: process.stdin,
        output: process.stdout
    });
    
    console.log("欢迎来到神秘迷宫寻宝游戏！");
    console.log("=".repeat(50));
    console.log("你将扮演一个勇敢的冒险者，探索神秘的迷宫并寻找宝藏！");
    
    game.player = new Player("冒险者");
    console.log(`你好，勇敢的 ${game.player.name}！`);
    console.log("你的目标是探索迷宫，收集宝藏，并安全地逃离。");
    console.log(`你拥有 ${game.player.hp} 点生命值。`);
    console.log("=".repeat(50));
    
    console.log("\n游戏开始！输入 'help' 查看可用命令。");
    
    function processCommand() {
        rl.question("\n> ", (input) => {
            const command = input.trim().toLowerCase();
            
            if (command === "quit") {
                console.log("感谢游玩神秘迷宫寻宝游戏！");
                rl.close();
                return;
            } else if (command === "help") {
                game.showHelp();
                processCommand();
            } else if (command === "look") {
                game.showRoomInfo();
                processCommand();
            } else if (command === "status") {
                game.showPlayerStatus();
                processCommand();
            } else if (command === "inventory") {
                game.showPlayerStatus();
                processCommand();
            } else if (command === "take") {
                game.pickItem();
                processCommand();
            } else if (command === "fight") {
                game.combat();
                processCommand();
            } else if (command === "puzzle") {
                game.solvePuzzle();
                processCommand();
            } else if (command.startsWith("move ")) {
                const direction = command.substring(5);
                game.movePlayer(direction);
                processCommand();
            } else {
                console.log("无效的命令。输入 'help' 查看可用命令。");
                processCommand();
            }
            
            // 检查游戏结束条件
            if (game.player.treasuresFound >= 2) {
                console.log("\n恭喜！你收集了足够的宝藏，成功逃离了迷宫！");
                console.log("你是一个真正的冒险者！");
                rl.close();
                return;
            }
            
            if (game.player.hp <= 0) {
                console.log("\n游戏结束！你被击败了...");
                rl.close();
                return;
            }
        });
    }
    
    processCommand();
}

module.exports = MazeGame;