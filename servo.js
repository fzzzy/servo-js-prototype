
let result = connect("127.0.0.1", 6000);

print("Opened socket", result);

yield result.send("hello world");
let echo = yield result.recv(4096);

print("echo", echo);

result.close();

setTimeout(function(foo) {
    print("timer!", foo);
}, 0.1, "foo");

let foo = new XMLHttpRequest();
foo.onreadystatechange = function () {
    if (this.readyState === 4) {
        print(this.responseText);
    }
}

foo.open("GET", 'http://localhost/');
foo.send();
