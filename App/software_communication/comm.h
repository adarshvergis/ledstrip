/**
 * @file comm.h
 * @brief Communication with the host software: protocol command dispatcher.
 */
#ifndef COMM_H
#define COMM_H

/** Start the network (from the stored settings) and register the command handler. */
void comm_init(void);

/** Periodic housekeeping (firmware update timeout). Called by the supervisor. */
void comm_service(void);

/** Register the debug CLI commands (no-op without ENABLE_DEBUG_CLI). */
void cli_commands_register(void);

#endif /* COMM_H */
