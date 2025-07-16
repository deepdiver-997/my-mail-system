#include <iostream>
using namespace std;

int main(){
    string s = "Hello, World!\n\nThis is a test.";
    auto pos = s.find("\n\n");
    cout << s[pos + 2];
    return 0;
}