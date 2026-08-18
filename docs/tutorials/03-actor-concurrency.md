# Tutorial 3: Actor-Based Concurrency

**Time:** 2 hours
**Prerequisites:** Tutorials 1 & 2
**Goal:** Learn to write concurrent programs with actors

## What Are Actors?

Actors are independent units of computation that:
- Have their own **state** (private data)
- Process **messages** one at a time
- Communicate by **sending messages** (not sharing memory)
- Run **concurrently** without manual thread management

Think of actors like people sending letters to each other - they don't share notebooks, they send messages!

## Your First Actor

### Simple Counter Actor

```aether
message Increment {}

actor Counter {
    state count = 0

    receive {
        Increment() -> {
            count = count + 1
        }
    }
}

main() {
    // Create a counter
    counter = spawn(Counter())

    // Send it messages
    counter ! Increment {}
    counter ! Increment {}
    counter ! Increment {}

    print("Counter processed 3 messages!\n")
}
```

**What's happening:**
1. `message Increment {}` defines a message type
2. `spawn(Counter())` creates a new counter actor instance
3. `counter ! Increment {}` sends an Increment message to the counter
4. The `receive` block pattern-matches incoming messages
5. Each Increment message increments the count

## Actor Components

### 1. State Variables

State persists across messages:

```aether,fragment
message Deposit { amount: int }
message Withdraw { amount: int }

actor BankAccount {
    state balance = 1000
    state account_number = 12345

    receive {
        Deposit(amount) -> {
            balance = balance + amount
        }
        Withdraw(amount) -> {
            balance = balance - amount
        }
    }
}
```

### 2. Message Definitions

Messages are defined with the `message` keyword and can carry typed fields:

```aether,fragment
// Empty message (no data)
message Reset {}

// Message with one field
message SetValue { value: int }

// Message with multiple fields
message Transfer { amount: int }
```

Messages are matched in the `receive` block using pattern syntax:

```aether
message Add { value: int }
message Multiply { value: int }
message Reset {}

actor Processor {
    state total = 0

    receive {
        Add(value) -> {
            total = total + value
        }
        Multiply(value) -> {
            total = total * value
        }
        Reset() -> {
            total = 0
        }
    }
}

main() {
    proc = spawn(Processor())

    proc ! Add { value: 10 }       // Add 10
    proc ! Add { value: 5 }        // Add 5
    proc ! Multiply { value: 2 }   // Multiply by 2
}
```

### 3. Message Processing

Messages are processed **one at a time** in the order received:

```aether
message LogEvent { level: int }

actor Logger {
    state message_count = 0

    receive {
        LogEvent(level) -> {
            message_count = message_count + 1
            print("Message #")
            print(message_count)
            print(": level=")
            print(level)
            print("\n")
        }
    }
}

main() {
    logger = spawn(Logger())

    // Send 3 messages
    logger ! LogEvent { level: 1 }
    logger ! LogEvent { level: 2 }
    logger ! LogEvent { level: 3 }
}
```

## Practical Examples

### Example 1: Shopping Cart

```aether
message AddItem { price: int }
message RemoveItem { price: int }
message ClearCart {}

actor ShoppingCart {
    state item_count = 0
    state total_price = 0

    receive {
        AddItem(price) -> {
            item_count = item_count + 1
            total_price = total_price + price
        }
        RemoveItem(price) -> {
            if (item_count > 0) {
                item_count = item_count - 1
                total_price = total_price - price
            }
        }
        ClearCart() -> {
            item_count = 0
            total_price = 0
        }
    }
}

main() {
    cart = spawn(ShoppingCart())

    // Add items
    cart ! AddItem { price: 100 }   // $100 item
    cart ! AddItem { price: 50 }    // $50 item
    cart ! AddItem { price: 75 }    // $75 item

    print("Shopping cart ready!\n")
}
```

### Example 2: Traffic Light

```aether
message ChangeLight {}
message Tick {}

actor TrafficLight {
    state current_light = 1  // 1=red, 2=yellow, 3=green
    state duration = 0

    receive {
        ChangeLight() -> {
            if (current_light == 1) {
                current_light = 3  // red -> green
            } else {
                if (current_light == 3) {
                    current_light = 2  // green -> yellow
                } else {
                    current_light = 1  // yellow -> red
                }
            }
        }
        Tick() -> {
            duration = duration + 1
        }
    }
}

main() {
    light = spawn(TrafficLight())

    // Simulate time passing
    light ! Tick {}
    light ! ChangeLight {}
    light ! Tick {}
    light ! ChangeLight {}
}
```

### Example 3: Multiple Actors

```aether
message Increment {}

actor Counter {
    state count = 0

    receive {
        Increment() -> {
            count = count + 1
        }
    }
}

main() {
    // Create multiple counters
    counter1 = spawn(Counter())
    counter2 = spawn(Counter())
    counter3 = spawn(Counter())

    // Send messages to different counters
    counter1 ! Increment {}
    counter1 ! Increment {}
    counter2 ! Increment {}
    counter3 ! Increment {}
    counter3 ! Increment {}
    counter3 ! Increment {}

    print("Multiple actors running!\n")
}
```

## Actor Design Patterns

### Pattern 1: Request-Response

```aether,fragment
message AddTen { value: int }
message Double { value: int }

actor Calculator {
    state last_result = 0

    receive {
        AddTen(value) -> {
            last_result = value + 10
        }
        Double(value) -> {
            last_result = value * 2
        }
    }
}
```

### Pattern 2: State Machine

```aether,fragment
message TakeDamage { amount: int }
message Heal { amount: int }
message LevelUp {}

actor GamePlayer {
    state health = 100
    state alive = 1
    state level = 1

    receive {
        TakeDamage(amount) -> {
            if (alive == 1) {
                health = health - amount
                if (health <= 0) {
                    alive = 0
                }
            }
        }
        Heal(amount) -> {
            if (alive == 1) {
                health = health + amount
                if (health > 100) {
                    health = 100
                }
            }
        }
        LevelUp() -> {
            if (alive == 1) {
                level = level + 1
                health = 100
            }
        }
    }
}
```

### Pattern 3: Accumulator

```aether,fragment
message AddValue { value: int }

actor Statistics {
    state total = 0
    state count = 0
    state max = 0

    receive {
        AddValue(value) -> {
            total = total + value
            count = count + 1

            if (value > max) {
                max = value
            }
        }
    }
}
```

## Exercises

### Exercise 1: Temperature Sensor

Create an actor that tracks temperature readings:

```aether,fragment
message UpdateTemp { temp: int }
message Reading {}

actor TempSensor {
    state current_temp = 20
    state readings = 0

    receive {
        UpdateTemp(temp) -> {
            current_temp = temp
            readings = readings + 1
        }
        Reading() -> {
            // Print current temperature
        }
    }
}
```

### Exercise 2: Inventory Manager

Create an actor for game inventory:

```aether,fragment
message AddGold { amount: int }
message SpendGold { amount: int }
message AddItem {}
message RemoveItem {}

actor Inventory {
    state gold = 0
    state items = 0

    receive {
        AddGold(amount) -> {
            gold = gold + amount
        }
        SpendGold(amount) -> {
            gold = gold - amount
        }
        AddItem() -> {
            items = items + 1
        }
        RemoveItem() -> {
            items = items - 1
        }
    }
}
```

### Exercise 3: Score Tracker

Create an actor that tracks game scores:

```aether,fragment
message AddPoints { points: int }
message SetMultiplier { value: int }
message ResetScore {}

actor ScoreTracker {
    state score = 0
    state multiplier = 1
    state high_score = 0

    receive {
        AddPoints(points) -> {
            score = score + points * multiplier
            if (score > high_score) {
                high_score = score
            }
        }
        SetMultiplier(value) -> {
            multiplier = value
        }
        ResetScore() -> {
            score = 0
            multiplier = 1
        }
    }
}
```

## Why Use Actors?

### Benefits

1. **No Race Conditions** - State is private, no shared memory
2. **Easy Concurrency** - No manual locks or mutexes
3. **Scalable** - Each actor is independent
4. **Isolated Failures** - A panicking actor takes down its own mailbox, not its callers' state

### When to Use Actors

- Independent concurrent tasks
- State machines
- Event processing
- Game entities (players, NPCs, items)
- Microservices patterns

### When NOT to Use Actors

- Simple sequential code
- High-speed data sharing
- Tight coupling between components

## Best Practices

### 1. Keep State Small

```aether,fragment
// Good: Minimal state
actor Counter {
    state count = 0
}

// Avoid: Too much state
actor Everything {
    state count = 0
    state name = ""
    state data1 = 0
    state data2 = 0
    // ... 20 more fields
}
```

### 2. Use Clear Message Names

```aether,fragment
// Define descriptive message types
message Increment {}
message Decrement {}
message Reset {}

actor Counter {
    state count = 0

    receive {
        Increment() -> {
            count = count + 1
        }
        Decrement() -> {
            count = count - 1
        }
        Reset() -> {
            count = 0
        }
    }
}
```

### 3. Send and Forget

The scheduler automatically delivers and processes messages:

```aether,fragment
// Just send - no manual stepping needed
counter ! Increment {}
counter ! Increment {}
counter ! Increment {}
```

## Waiting for Completion

When you need to wait for actors to finish processing before continuing, use `wait_for_idle()`:

```aether
message Compute { value: int }

actor Worker {
    state result = 0

    receive {
        Compute(value) -> {
            result = value * 2
        }
    }
}

main() {
    worker = spawn(Worker())

    // Send work
    worker ! Compute { value: 21 }

    // Wait for all messages to be processed
    wait_for_idle()

    // Now safe to read state
    print("Result: ")
    print(worker.result)
    print("\n")
}
```

The `wait_for_idle()` function blocks until all actors have finished processing their queued messages. This is useful when you need to:

- Read actor state after computation
- Coordinate multiple actors before continuing
- Ensure all work completes before program exit

For timed delays, use `sleep(milliseconds)`:

```aether,fragment
main() {
    worker = spawn(Worker())
    worker ! Start {}

    // Wait for 1 second
    sleep(1000)

    worker ! Stop {}
}
```

## Next Steps

You've learned:
- What actors are
- Actor state and messages
- Spawning and messaging
- Multiple concurrent actors
- Actor design patterns

**Next:** See the [Language Reference](../language-reference.md) for complete syntax details.

## Quick Reference

```aether,fragment
// Define messages
message MyMessage { value: int }
message SimpleMessage {}

// Define actor
actor MyActor {
    state value = 0

    receive {
        MyMessage(value) -> {
            // Handle message with data
        }
        SimpleMessage() -> {
            // Handle empty message
        }
    }
}

// Use actor
a = spawn(MyActor())
a ! MyMessage { value: 42 }
a ! SimpleMessage {}
```

Happy concurrent programming!
