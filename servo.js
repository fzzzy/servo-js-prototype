
let result = connect("127.0.0.1", 6000);

print(result);

yield result.send("hello world");
let echo = yield result.recv(4096);

print("echo", echo);

result.close();

setTimeout(function(foo) {
    print("timer!", foo);
}, 1, "foo");