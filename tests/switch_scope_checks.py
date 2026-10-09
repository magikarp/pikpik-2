"""Execute transformed C++ to check nested cases, directives and fall-through."""
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from scope_switches import scope_switches

source = r'''
int choose(int key) {
    int result = 0;
    switch (key) {
    case 0:
    case 1:
        int local = 10;
        switch (key) {
        case 0: local += 2; break;
        default: local += 3; break;
        }
        result = local;
        break;
    case 2:
#if 1
        int local = 20;
        result = local;
#endif
        break;
    case 3: result = 30; // intentional fall-through
    case 4: result += 4; break;
    default: return -1;
    }
    return result;
}
int main() {
    return !(choose(0)==12 && choose(1)==13 && choose(2)==20 &&
             choose(3)==34 && choose(4)==4 && choose(9)==-1);
}
'''
converted = scope_switches(source)
assert scope_switches(converted) == converted, "Transformation must be idempotent"
assert "case 3: result = 30;" in converted, "Fall-through must remain unscoped"
path = Path(sys.argv[2]).resolve() / "switch-check"
path.mkdir(exist_ok=True)
(path / "cases.cpp").write_text(converted)
subprocess.run([sys.argv[1], "-std=c++11", str(path / "cases.cpp"), "-o", str(path / "cases")], check=True)
subprocess.run([str(path / "cases")], check=True)
