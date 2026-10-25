# End-to-end test of the real paysphere executable.
#
# Feeds scripted keyboard input to three separate runs of the program on one
# database file, so the test covers the CLI, the services and persistence
# across restarts exactly as a user would exercise them.
#
# Run by CTest: cmake -DPAYSPHERE_EXE=... -DSCRIPT_DIR=... -DWORK_DIR=... -P cli_e2e.cmake
# Each run's transcript is kept in WORK_DIR for inspection.

foreach(variable PAYSPHERE_EXE SCRIPT_DIR WORK_DIR)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} must be set")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(DATABASE "${WORK_DIR}/cli-e2e.db")

# Runs one session; leaves the transcript in OUTPUT and checks the exit code.
function(run_session name)
    execute_process(
        COMMAND "${PAYSPHERE_EXE}" --db "${DATABASE}"
        INPUT_FILE "${WORK_DIR}/${name}.txt"
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
        RESULT_VARIABLE code
        TIMEOUT 300)
    file(WRITE "${WORK_DIR}/${name}.transcript.txt" "${out}${err}")
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "${name}: exit code ${code}\n${out}${err}")
    endif()
    set(OUTPUT "${out}${err}" PARENT_SCOPE)
    set(SESSION "${name}" PARENT_SCOPE)
endfunction()

function(expect text)
    string(FIND "${OUTPUT}" "${text}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${SESSION}: expected output to contain\n  ${text}\n"
                            "Transcript: ${WORK_DIR}/${SESSION}.transcript.txt")
    endif()
endfunction()

function(reject text)
    string(FIND "${OUTPUT}" "${text}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR "${SESSION}: output must not contain\n  ${text}\n"
                            "Transcript: ${WORK_DIR}/${SESSION}.transcript.txt")
    endif()
endfunction()

# `later` must appear after `earlier`.
function(expect_order earlier later)
    string(FIND "${OUTPUT}" "${earlier}" first)
    string(FIND "${OUTPUT}" "${later}" second REVERSE)
    if(first EQUAL -1 OR second EQUAL -1 OR NOT second GREATER first)
        message(FATAL_ERROR "${SESSION}: expected\n  ${later}\nafter\n  ${earlier}")
    endif()
endfunction()

# Secrets must never be echoed, and nothing may crash.
function(check_common)
    reject("correct horse battery")
    reject("wrong password")
    reject("PIN: 1357")
    reject("PIN: 2468")
    reject("PIN: 2580")
    reject("Unexpected error")
    expect("Goodbye.")
endfunction()

# --- Session 1: a new installation ------------------------------------------
configure_file("${SCRIPT_DIR}/session1.txt" "${WORK_DIR}/session1.txt" COPYONLY)
run_session(session1)
check_common()
expect("schema v6")
expect("Registered Asha Verma (asha@paysphere). A wallet with Rs. 0.00 was created.")
expect("Registered Ravi Kumar (ravi@paysphere).")
expect("Error: An account with this email already exists")
expect("Error: the passwords do not match.")
expect("Error: Incorrect email/UPI ID or password")
expect("Welcome, Asha Verma.")
expect("Error: Set your UPI PIN before making payments")
expect("UPI PIN set.")
expect("Error: Invalid amount")
expect("Deposited Rs. 1,000.00.")
expect("Paid Rs. 250.50 to ravi@paysphere.")
expect("Payment FAILED: Insufficient balance.")
expect("Payment FAILED: Recipient UPI ID not found.")
expect("Payment FAILED: Incorrect UPI PIN.")
expect("Please choose one of the listed options.")
expect("Balance: Rs. 749.50")
expect("Showing 5 of 5 transaction(s)")
expect("Logged out.")
expect_order("Logged out." "Welcome, Ravi Kumar.")
expect("Balance: Rs. 250.50")
string(REGEX MATCH "Paid Rs\\. 250\\.50 to ravi@paysphere\\. Transaction ID (TRF[0-9A-F]+)" match "${OUTPUT}")
set(TRANSFER_ID "${CMAKE_MATCH_1}")
if(TRANSFER_ID STREQUAL "")
    message(FATAL_ERROR "session1: could not find the transfer's transaction ID")
endif()

# --- Session 2: after a restart ---------------------------------------------
configure_file("${SCRIPT_DIR}/session2.txt" "${WORK_DIR}/session2.txt" COPYONLY)
run_session(session2)
check_common()
expect("Welcome, Asha Verma.")
expect("Balance: Rs. 749.50")                          # balance persisted
expect("Showing 4 of 4 transaction(s)")                 # asha's sent history persisted
expect("${TRANSFER_ID}")
expect("Error: No transaction with this ID was found in your account")
expect("Error: A UPI PIN is already set for this account") # PIN persisted
expect("Welcome, Ravi Kumar.")
expect("Showing 1 of 1 transaction(s)")                 # ravi does not see asha's failures
expect("asha@paysphere")

# --- Session 3: receipts and access control; input ends without "0" -------
file(WRITE "${WORK_DIR}/session3.txt"
    "2\nravi@example.com\ncorrect horse battery\n5\n${TRANSFER_ID}\n7\n"
    "1\nMeera Shah\nmeera@example.com\n9000000003\nmeera@paysphere\ncorrect horse battery\ncorrect horse battery\n"
    "2\nmeera@paysphere\ncorrect horse battery\n5\n${TRANSFER_ID}\n")
run_session(session3)
check_common()
expect("Transaction ID  ${TRANSFER_ID}")
expect("Type            UPI transfer (received)")
expect("From            Asha Verma <asha@paysphere>")
expect("Amount          Rs. 250.50")
expect("Registered Meera Shah")
expect_order("Welcome, Meera Shah." "Error: No transaction with this ID was found in your account")

message(STATUS "CLI end-to-end test passed (transfer ${TRANSFER_ID}); transcripts in ${WORK_DIR}")
