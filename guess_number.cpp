#include <iostream>
#include <cstdlib>
#include <ctime>
#include <string>

using namespace std;

int main() {
    // 初始化随机数种子
    srand(time(0));
    
    // 生成1-100之间的随机数
    int target = rand() % 100 + 1;
    int guess = 0;
    int attempts = 0;
    
    cout << "欢迎来到猜数游戏！" << endl;
    cout << "我已经想好了一个1到100之间的数字。" << endl;
    cout << "请开始猜测这个数字是多少：" << endl;
    
    while (true) {
        cout << "请输入你的猜测: ";
        cin >> guess;
        attempts++;
        
        if (guess < target) {
            cout << "太小了！再试一次。" << endl;
        } else if (guess > target) {
            cout << "太大了！再试一次。" << endl;
        } else {
            cout << "恭喜你！猜对了！" << endl;
            cout << "你用了 " << attempts << " 次猜测。" << endl;
            break;
        }
    }
    
    cout << "游戏结束！感谢参与！" << endl;
    return 0;
}