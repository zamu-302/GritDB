class hello {
public:
  int x;
  int y;
  hello(int x, int y) {
    this->x = x;
    this->y = y;
  }
};

int main() {

  hello name(1, 2);
  if (dectype(name) == typeid(hello)) {
    std::cout << "nice";
    return 1;
  }

  return 0;
}
