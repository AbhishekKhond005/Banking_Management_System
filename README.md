# Banking Management System (BMS)

A robust, C-based Banking Management System designed for reliability and concurrency. This system features a client-server architecture supporting multiple roles (Customer, Employee, Manager, Admin) with secure password handling and transaction logging.

## Features

- **Role-Based Access Control**:
  - **Customer**: View balance, deposit, withdraw, transfer, apply for loans, view history.
  - **Employee**: Add customers, view transactions, approve/reject loans.
  - **Manager**: Activate/deactivate accounts, assign loans, review feedback.
  - **Admin**: Manage employees and roles.
- **Concurrency**: Handles multiple clients simultaneously using threads.
- **Persistence**: Custom file-based database for users, accounts, loans, and transactions.
- **Security**: Password hashing (simple implementation) to protect user credentials.
- **Transaction Logging**: Detailed logs of all financial activities.

## Getting Started

### Prerequisites
- GCC Compiler
- Linux Environment (for pthread and socket libraries)

### Building the Project
Run the following command in the project root:
```bash
make
```
This will generate two executables: `server` and `client`.

### Running the System

1. **Start the Server**:
   ```bash
   ./server <port>
   # Example:
   ./server 8080
   ```

2. **Start a Client**:
   ```bash
   ./client <server_ip> <port>
   # Example:
   ./client 127.0.0.1 8080
   ```

### Initial Login
The system initializes with a default admin account:
- **Username**: `admin`
- **Password**: `admin`

*Note: It is highly recommended to change the admin password immediately after the first login.*

## Project Structure

- `server.c`: Handles client connections and dispatches commands.
- `client.c`: User interface for interacting with the server.
- `db.c`: Database operations (file I/O, locking, logic).
- `common.h`: Shared definitions and structures.
- `Makefile`: Build configuration.

## License
This project is for educational purposes.
