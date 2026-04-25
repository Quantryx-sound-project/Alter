#pragma once
#include <JuceHeader.h>
#include <array>
#include <cstring>

// ---------------------------------------------------------------
// MIDI Handler - spracovanie MIDI eventov z audio vlakna
// ---------------------------------------------------------------
// ALTM packet format:
// [Header 'ALTM'] (4 bytes)
// [activeNoteCount] (1 byte) - počet aktívnych not
// [noteNumber_1, velocity_1, noteNumber_2, velocity_2, ...] (2 bytes per note)
// Max 64 not = max packet: 4 + 1 + 64*2 = 133 bytes

class MidiHandler
{
public:
    static constexpr int kMaxActiveNotes = 64;
    static constexpr int kMaxPacketSize = 4 + 1 + kMaxActiveNotes * 2;

    MidiHandler() = default;

    // Volane z audio vlakna - spracovanie MIDI bufferu z processBlock
    void processMidiBuffer (const juce::MidiBuffer& midiMessages)
    {
        // Spracuj všetky MIDI events
        for (const auto midiData : midiMessages)
        {
            const auto msg = midiData.getMessage();

            if (msg.isNoteOn())
            {
                addNote (msg.getNoteNumber(), msg.getVelocity());
            }
            else if (msg.isNoteOff() || (msg.isNoteOn() && msg.getVelocity() == 0))
            {
                removeNote (msg.getNoteNumber());
            }
        }
    }

    // Serializujem aktívne noty do ALTM packetu
    void serializeToPacket (uint8_t* outPacket, int& outPacketSize)
    {
        // Header: 'ALTM'
        outPacket[0] = 'A';
        outPacket[1] = 'L';
        outPacket[2] = 'T';
        outPacket[3] = 'M';

        // Počet aktívnych not
        uint8_t noteCount = (uint8_t) activeNotes.size();
        outPacket[4] = noteCount;

        // Zapis noty a ich velocity
        int offset = 5;
        for (size_t i = 0; i < activeNotes.size() && i < (size_t) kMaxActiveNotes; ++i)
        {
            outPacket[offset + 0] = activeNotes[i].noteNumber;
            outPacket[offset + 1] = activeNotes[i].velocity;
            offset += 2;
        }

        outPacketSize = offset;
    }

private:
    struct Note
    {
        uint8_t noteNumber = 0;
        uint8_t velocity = 0;

        bool operator== (uint8_t nn) const { return noteNumber == nn; }
    };

    std::vector<Note> activeNotes;

    void addNote (int noteNumber, int velocity)
    {
        // Kontrola, či nota už existuje
        for (auto& note : activeNotes)
        {
            if (note.noteNumber == (uint8_t) noteNumber)
            {
                note.velocity = (uint8_t) velocity;
                return;
            }
        }

        // Ak je miesto, pridaj novú notu
        if (activeNotes.size() < (size_t) kMaxActiveNotes)
        {
            activeNotes.push_back ({ (uint8_t) noteNumber, (uint8_t) velocity });
        }
    }

    void removeNote (int noteNumber)
    {
        auto it = std::find (activeNotes.begin(), activeNotes.end(), (uint8_t) noteNumber);
        if (it != activeNotes.end())
        {
            activeNotes.erase (it);
        }
    }
};
